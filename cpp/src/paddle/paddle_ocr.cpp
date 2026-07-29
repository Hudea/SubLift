#include "sublift/paddle.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <queue>
#include <stdexcept>
#include <vector>

#include "paddle_models.hpp"
#include "ppocr_crop_cls.hpp"
#include "ppocr_ctc.hpp"
#include "ppocr_db_postprocess.hpp"
#include "sublift/image.hpp"

#if defined(SUBLIFT_HAS_PADDLE) && SUBLIFT_HAS_PADDLE
#include <onnxruntime_cxx_api.h>

#if defined(SUBLIFT_HAS_OPENCV) || __has_include(<opencv2/opencv.hpp>)
#include <opencv2/opencv.hpp>
#define SUBLIFT_PADDLE_USE_OPENCV 1
#endif
#endif

namespace sublift {

#if defined(SUBLIFT_HAS_PADDLE) && SUBLIFT_HAS_PADDLE

namespace {

std::filesystem::path resolve_keys_path(const paddle_detail::ModelPaths& paths) {
  if (std::filesystem::exists(paths.keys_path)) {
    return paths.keys_path;
  }
  auto alt = paths.keys_path.parent_path() / "ppocrv6_dict.txt";
  if (std::filesystem::exists(alt)) {
    return alt;
  }
  return paths.keys_path;
}

std::vector<std::string> load_dictionary(const std::filesystem::path& keys_path) {
  std::ifstream in(keys_path);
  if (!in) {
    throw std::runtime_error("无法读取 Paddle 字典: " + keys_path.string());
  }
  std::vector<std::string> dict;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    dict.push_back(line);
  }
  // rapidocr inserts a space character after the file contents for CTC.
  dict.emplace_back(" ");
  if (dict.empty()) {
    throw std::runtime_error("Paddle 字典为空: " + keys_path.string());
  }
  return dict;
}

void rgb_to_nchw_norm(const uint8_t* rgb, int32_t w, int32_t h, float* out) {
  const int32_t plane = w * h;
  for (int32_t y = 0; y < h; ++y) {
    for (int32_t x = 0; x < w; ++x) {
      const uint8_t* p = rgb + (y * w + x) * 3;
      const int32_t idx = y * w + x;
      out[0 * plane + idx] = static_cast<float>(p[0]) / 255.0f * 2.0f - 1.0f;
      out[1 * plane + idx] = static_cast<float>(p[1]) / 255.0f * 2.0f - 1.0f;
      out[2 * plane + idx] = static_cast<float>(p[2]) / 255.0f * 2.0f - 1.0f;
    }
  }
}

}  // namespace

bool is_paddle_available() noexcept {
#if !SUBLIFT_HAS_PADDLE
  return false;
#else
  try {
    auto dir = paddle_detail::resolve_model_dir("");
    auto paths =
        paddle_detail::get_expected_model_paths(dir, paddle_detail::ModelType::Small);
    return paddle_detail::validate_model_paths(paths, nullptr);
  } catch (...) {
    return false;
  }
#endif
}

struct PaddleOcrEngine::Impl {
  PaddleOcrOptions options;
  paddle_detail::ModelType model_type;
  std::filesystem::path model_dir;
  paddle_detail::ModelPaths model_paths;
  std::vector<std::string> dictionary;

  Ort::Env env;
  Ort::SessionOptions session_options;
  std::unique_ptr<Ort::Session> det_session;
  std::unique_ptr<Ort::Session> rec_session;

  std::string det_input_name;
  std::string det_output_name;
  std::string rec_input_name;
  std::string rec_output_name;

  Impl(PaddleOcrOptions opts)
      : options(std::move(opts)),
        model_type(paddle_detail::parse_model_type(options.model_type)),
        model_dir(paddle_detail::resolve_model_dir(options.model_root_dir)),
        model_paths(paddle_detail::get_expected_model_paths(model_dir, model_type)),
        dictionary(load_dictionary(resolve_keys_path(model_paths))),
        env(ORT_LOGGING_LEVEL_WARNING, "sublift_paddle") {
    paddle_detail::validate_model_paths(model_paths, nullptr);

    session_options.SetIntraOpNumThreads(1);
    session_options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

    det_session = std::make_unique<Ort::Session>(env, model_paths.det_path.c_str(), session_options);
    rec_session = std::make_unique<Ort::Session>(env, model_paths.rec_path.c_str(), session_options);

    Ort::AllocatorWithDefaultOptions alloc;
    {
      auto in = det_session->GetInputNameAllocated(0, alloc);
      det_input_name = in.get();
      auto out = det_session->GetOutputNameAllocated(0, alloc);
      det_output_name = out.get();
    }
    {
      auto in = rec_session->GetInputNameAllocated(0, alloc);
      rec_input_name = in.get();
      auto out = rec_session->GetOutputNameAllocated(0, alloc);
      rec_output_name = out.get();
    }
  }

  paddle::DBPostProcessResult run_det(const uint8_t* rgb, int32_t w, int32_t h, int32_t stride) {
    constexpr int limit_side = 960;
    const int max_side = std::max(w, h);
    float ratio = 1.0f;
    if (max_side > limit_side) {
      ratio = static_cast<float>(limit_side) / static_cast<float>(max_side);
    }
    int32_t rw = static_cast<int32_t>(std::round(w * ratio));
    int32_t rh = static_cast<int32_t>(std::round(h * ratio));
    rw = std::max(32, static_cast<int32_t>(std::round(rw / 32.0) * 32));
    rh = std::max(32, static_cast<int32_t>(std::round(rh / 32.0) * 32));

    std::vector<uint8_t> resized(static_cast<size_t>(rw * rh * 3));

#if defined(SUBLIFT_PADDLE_USE_OPENCV)
    cv::Mat src_img(h, w, CV_8UC3, const_cast<uint8_t*>(rgb), stride);
    cv::Mat dst_img;
    cv::resize(src_img, dst_img, cv::Size(rw, rh), 0, 0, cv::INTER_LINEAR);
    cv::Mat bgr_mat;
    cv::cvtColor(dst_img, bgr_mat, cv::COLOR_RGB2BGR);
    std::vector<float> input(static_cast<size_t>(3 * rw * rh));
    rgb_to_nchw_norm(bgr_mat.data, rw, rh, input.data());
#else
    for (int32_t y = 0; y < rh; ++y) {
      const int32_t sy = std::min(h - 1, static_cast<int32_t>((static_cast<int64_t>(y) * h) / rh));
      for (int32_t x = 0; x < rw; ++x) {
        const int32_t sx = std::min(w - 1, static_cast<int32_t>((static_cast<int64_t>(x) * w) / rw));
        const uint8_t* sp = rgb + sy * stride + sx * 3;
        uint8_t* dp = resized.data() + (y * rw + x) * 3;
        dp[0] = sp[0]; dp[1] = sp[1]; dp[2] = sp[2];
      }
    }
    std::vector<uint8_t> bgr(resized.size());
    rgb24_to_bgr24(resized.data(), bgr.data(), rw, rh, rw * 3, rw * 3);
    std::vector<float> input(static_cast<size_t>(3 * rw * rh));
    rgb_to_nchw_norm(bgr.data(), rw, rh, input.data());
#endif

    std::array<int64_t, 4> shape{1, 3, rh, rw};
    Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    Ort::Value tensor = Ort::Value::CreateTensor<float>(mem, input.data(), input.size(),
                                                        shape.data(), shape.size());
    const char* in_names[] = {det_input_name.c_str()};
    const char* out_names[] = {det_output_name.c_str()};
    auto outputs = det_session->Run(Ort::RunOptions{nullptr}, in_names, &tensor, 1, out_names, 1);
    float* out = outputs[0].GetTensorMutableData<float>();
    auto info = outputs[0].GetTensorTypeAndShapeInfo();
    auto dims = info.GetShape();
    int32_t oh = rh;
    int32_t ow = rw;
    if (dims.size() == 4) {
      oh = static_cast<int32_t>(dims[2]);
      ow = static_cast<int32_t>(dims[3]);
    } else if (dims.size() == 3) {
      oh = static_cast<int32_t>(dims[1]);
      ow = static_cast<int32_t>(dims[2]);
    }
    paddle::DBPostProcessOptions opts;
    return paddle::db_postprocess(out, oh, ow, h, w, opts);
  }

  paddle_detail::CtcResult run_rec_crop(const paddle::CropResult& crop) {
    if (crop.width <= 0 || crop.height <= 0 || crop.rgb_data.empty()) {
      return paddle_detail::CtcResult{};
    }

    constexpr int32_t rec_h = 48;
    const float wh_ratio = static_cast<float>(crop.width) / static_cast<float>(std::max(1, crop.height));
    int32_t rec_w = static_cast<int32_t>(std::ceil(rec_h * wh_ratio));
    rec_w = std::max(rec_w, 8);
    rec_w = std::min(rec_w, 3200);

    std::vector<float> input_tensor(static_cast<size_t>(3 * rec_h * rec_w), 0.0f);
    paddle::prepare_rec_tensor(crop, rec_h, rec_w, input_tensor.data());

    std::array<int64_t, 4> shape{1, 3, rec_h, rec_w};
    Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    Ort::Value tensor = Ort::Value::CreateTensor<float>(mem, input_tensor.data(), input_tensor.size(),
                                                        shape.data(), shape.size());
    const char* in_names[] = {rec_input_name.c_str()};
    const char* out_names[] = {rec_output_name.c_str()};
    auto outputs = rec_session->Run(Ort::RunOptions{nullptr}, in_names, &tensor, 1, out_names, 1);
    float* out = outputs[0].GetTensorMutableData<float>();
    auto dims = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
    int seq_len = 0;
    int num_classes = 0;
    if (dims.size() == 3) {
      seq_len = static_cast<int>(dims[1]);
      num_classes = static_cast<int>(dims[2]);
      if (num_classes < 10 && dims[1] > dims[2]) {
        seq_len = static_cast<int>(dims[2]);
        num_classes = static_cast<int>(dims[1]);
      }
    } else {
      throw std::runtime_error("PaddleOCR rec 输出维度异常");
    }
    return paddle_detail::ctc_greedy_decode(out, seq_len, num_classes, dictionary);
  }
};

PaddleOcrEngine::PaddleOcrEngine(PaddleOcrOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

PaddleOcrEngine::~PaddleOcrEngine() = default;

PaddleOcrEngine::PaddleOcrEngine(PaddleOcrEngine&&) noexcept = default;
PaddleOcrEngine& PaddleOcrEngine::operator=(PaddleOcrEngine&&) noexcept = default;

OcrResult PaddleOcrEngine::recognize(const ImageView& image) {
  if (image.width() <= 0 || image.height() <= 0) {
    return OcrResult::from_lines({});
  }
  if (image.format() != PixelFormat::RGB24) {
    throw std::runtime_error("PaddleOCR 仅支持 RGB24 ImageView");
  }
  if (!impl_ || !impl_->det_session || !impl_->rec_session) {
    throw std::runtime_error("PaddleOCR 推理会话未初始化");
  }

  try {
    const uint8_t* rgb = image.data();
    const int32_t w = image.width();
    const int32_t h = image.height();
    const int32_t stride = static_cast<int32_t>(image.stride_bytes());

    auto db_res = impl_->run_det(rgb, w, h, stride);
    if (db_res.quads.empty()) {
      return OcrResult::from_lines({});
    }

    std::vector<OcrLine> lines;
    lines.reserve(db_res.quads.size());
    for (size_t i = 0; i < db_res.quads.size(); ++i) {
      const auto& quad = db_res.quads[i];
      const auto& aabb = (i < db_res.aabbs.size()) ? db_res.aabbs[i] : OcrCropBox{0, 0, w, h};

      paddle::CropResult crop = paddle::get_rotate_crop(rgb, w, h, stride, quad);
      auto ctc = impl_->run_rec_crop(crop);
      if (is_strip_empty(ctc.text) || ctc.confidence < 0.5f) {
        continue;
      }
      OcrCropBox crop_box = clamp_box(aabb, w, h);
      lines.push_back(OcrLine{
          .text = std::move(ctc.text),
          .confidence = ctc.confidence,
          .box = crop_box,
      });
    }
    sort_ocr_lines(lines);
    return OcrResult::from_lines(lines);
  } catch (const Ort::Exception& e) {
    throw std::runtime_error(std::string("PaddleOCR ONNX Runtime 推理故障: ") + e.what());
  } catch (const std::runtime_error&) {
    throw;
  } catch (const std::exception& e) {
    throw std::runtime_error(std::string("PaddleOCR 推理故障: ") + e.what());
  }
}

#else

bool is_paddle_available() noexcept {
  return false;
}

struct PaddleOcrEngine::Impl {};

PaddleOcrEngine::PaddleOcrEngine(PaddleOcrOptions options) {
  (void)paddle_detail::parse_model_type(options.model_type);
  throw std::runtime_error("PaddleOCR 不可用：C++ 构建未启用 SUBLIFT_ENABLE_PADDLE");
}

PaddleOcrEngine::~PaddleOcrEngine() = default;

PaddleOcrEngine::PaddleOcrEngine(PaddleOcrEngine&&) noexcept = default;
PaddleOcrEngine& PaddleOcrEngine::operator=(PaddleOcrEngine&&) noexcept = default;

OcrResult PaddleOcrEngine::recognize(const ImageView& /*image*/) {
  throw std::runtime_error("PaddleOCR 不可用：C++ 构建未启用 SUBLIFT_ENABLE_PADDLE");
}

#endif

}  // namespace sublift
