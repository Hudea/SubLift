#include "sublift/paddle.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <queue>
#include <stdexcept>
#include <vector>

#include "paddle_models.hpp"
#include "ppocr_ctc.hpp"
#include "ppocr_db_postprocess.hpp"
#include "sublift/image.hpp"

namespace sublift {

#if defined(SUBLIFT_HAS_PADDLE) && SUBLIFT_HAS_PADDLE

#include <onnxruntime_cxx_api.h>

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

// Nearest-neighbor resize RGB24 → packed RGB dst (w*h*3).
void resize_rgb_nn(const uint8_t* src, int32_t sw, int32_t sh, int32_t sstride,
                   uint8_t* dst, int32_t dw, int32_t dh) {
  for (int32_t y = 0; y < dh; ++y) {
    const int32_t sy = std::min(sh - 1, static_cast<int32_t>((static_cast<int64_t>(y) * sh) / dh));
    for (int32_t x = 0; x < dw; ++x) {
      const int32_t sx = std::min(sw - 1, static_cast<int32_t>((static_cast<int64_t>(x) * sw) / dw));
      const uint8_t* sp = src + sy * sstride + sx * 3;
      uint8_t* dp = dst + (y * dw + x) * 3;
      dp[0] = sp[0];
      dp[1] = sp[1];
      dp[2] = sp[2];
    }
  }
}

void rgb_to_nchw_norm(const uint8_t* rgb, int32_t w, int32_t h, float* out) {
  // (pixel/255 - 0.5) / 0.5  ==  pixel/255 * 2 - 1
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

struct DetBox {
  int32_t x0{0};
  int32_t y0{0};
  int32_t x1{0};
  int32_t y1{0};
  float score{0.0f};
};

// Simplified DB-like postprocess: threshold + connected components AABB.
// Not full unclip/polygon; enough for subtitle ROI strips and acceptance smoke.
std::vector<DetBox> boxes_from_prob_map(const float* map, int32_t mh, int32_t mw,
                                        int32_t orig_w, int32_t orig_h,
                                        float thresh = 0.3f, float box_thresh = 0.5f) {
  std::vector<uint8_t> bin(static_cast<size_t>(mh * mw), 0);
  for (int32_t i = 0; i < mh * mw; ++i) {
    bin[static_cast<size_t>(i)] = map[i] >= thresh ? 1 : 0;
  }

  std::vector<int32_t> labels(static_cast<size_t>(mh * mw), 0);
  int32_t next_label = 1;
  struct Comp {
    int32_t x0, y0, x1, y1;
    double score_sum;
    int count;
  };
  std::vector<Comp> comps;
  comps.emplace_back();  // index 0 unused

  auto idx = [mw](int32_t y, int32_t x) { return y * mw + x; };

  for (int32_t y = 0; y < mh; ++y) {
    for (int32_t x = 0; x < mw; ++x) {
      if (!bin[static_cast<size_t>(idx(y, x))] || labels[static_cast<size_t>(idx(y, x))] != 0) {
        continue;
      }
      Comp c{x, y, x, y, 0.0, 0};
      std::queue<std::pair<int32_t, int32_t>> q;
      q.emplace(y, x);
      labels[static_cast<size_t>(idx(y, x))] = next_label;
      while (!q.empty()) {
        auto [cy, cx] = q.front();
        q.pop();
        c.x0 = std::min(c.x0, cx);
        c.y0 = std::min(c.y0, cy);
        c.x1 = std::max(c.x1, cx);
        c.y1 = std::max(c.y1, cy);
        c.score_sum += map[idx(cy, cx)];
        c.count++;
        const int32_t nbs[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
        for (const auto& d : nbs) {
          const int32_t ny = cy + d[0];
          const int32_t nx = cx + d[1];
          if (ny < 0 || ny >= mh || nx < 0 || nx >= mw) continue;
          const int32_t ni = idx(ny, nx);
          if (!bin[static_cast<size_t>(ni)] || labels[static_cast<size_t>(ni)] != 0) continue;
          labels[static_cast<size_t>(ni)] = next_label;
          q.emplace(ny, nx);
        }
      }
      comps.push_back(c);
      ++next_label;
    }
  }

  const float sx = static_cast<float>(orig_w) / static_cast<float>(std::max(1, mw));
  const float sy = static_cast<float>(orig_h) / static_cast<float>(std::max(1, mh));
  std::vector<DetBox> out;
  for (size_t i = 1; i < comps.size(); ++i) {
    const auto& c = comps[i];
    if (c.count < 8) continue;
    const float mean_score = static_cast<float>(c.score_sum / std::max(1, c.count));
    if (mean_score < box_thresh) continue;
    DetBox b;
    b.x0 = std::max(0, static_cast<int32_t>(std::floor(c.x0 * sx)));
    b.y0 = std::max(0, static_cast<int32_t>(std::floor(c.y0 * sy)));
    b.x1 = std::min(orig_w, static_cast<int32_t>(std::ceil((c.x1 + 1) * sx)));
    b.y1 = std::min(orig_h, static_cast<int32_t>(std::ceil((c.y1 + 1) * sy)));
    // expand a bit for subtitle edges
    const int32_t pad_x = std::max(1, (b.x1 - b.x0) / 20);
    const int32_t pad_y = std::max(1, (b.y1 - b.y0) / 8);
    b.x0 = std::max(0, b.x0 - pad_x);
    b.y0 = std::max(0, b.y0 - pad_y);
    b.x1 = std::min(orig_w, b.x1 + pad_x);
    b.y1 = std::min(orig_h, b.y1 + pad_y);
    if (b.x1 - b.x0 < 2 || b.y1 - b.y0 < 2) continue;
    b.score = mean_score;
    out.push_back(b);
  }
  std::sort(out.begin(), out.end(), [](const DetBox& a, const DetBox& b) {
    if (a.y0 != b.y0) return a.y0 < b.y0;
    return a.x0 < b.x0;
  });
  return out;
}

}  // namespace

bool is_paddle_available() noexcept {
  try {
    auto dir = paddle_detail::resolve_model_dir("");
    auto paths =
        paddle_detail::get_expected_model_paths(dir, paddle_detail::ModelType::Small);
    return paddle_detail::validate_model_paths(paths, nullptr);
  } catch (...) {
    return false;
  }
}

struct PaddleOcrEngine::Impl {
  PaddleOcrOptions options;
  paddle_detail::ModelType model_type;
  std::filesystem::path model_dir;
  paddle_detail::ModelPaths model_paths;
  std::vector<std::string> dictionary;

  Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "SubLiftPaddle"};
  Ort::SessionOptions session_options;
  std::unique_ptr<Ort::Session> det_session;
  std::unique_ptr<Ort::Session> rec_session;
  std::string det_input_name;
  std::string det_output_name;
  std::string rec_input_name;
  std::string rec_output_name;

  explicit Impl(PaddleOcrOptions opts)
      : options(std::move(opts)),
        model_type(paddle_detail::parse_model_type(options.model_type)),
        model_dir(paddle_detail::resolve_model_dir(options.model_root_dir)),
        model_paths(paddle_detail::get_expected_model_paths(model_dir, model_type)) {
    session_options.SetIntraOpNumThreads(1);
    session_options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

    std::string err_msg;
    if (!paddle_detail::validate_model_paths(model_paths, &err_msg)) {
      throw std::runtime_error("PaddleOCR 不可用：" + err_msg +
                               "；请先通过 Python `uv sync --extra paddle` 下载模型，或设置 "
                               "SUBLIFT_PADDLE_MODEL_DIR。");
    }

    const auto keys = resolve_keys_path(model_paths);
    dictionary = load_dictionary(keys);

#if defined(_WIN32)
    const std::wstring det_w = model_paths.det_path.wstring();
    const std::wstring rec_w = model_paths.rec_path.wstring();
    det_session = std::make_unique<Ort::Session>(env, det_w.c_str(), session_options);
    rec_session = std::make_unique<Ort::Session>(env, rec_w.c_str(), session_options);
#else
    det_session =
        std::make_unique<Ort::Session>(env, model_paths.det_path.c_str(), session_options);
    rec_session =
        std::make_unique<Ort::Session>(env, model_paths.rec_path.c_str(), session_options);
#endif

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

  std::vector<DetBox> run_det(const uint8_t* rgb, int32_t w, int32_t h, int32_t stride) {
    // limit_type=min, limit_side_len=736 (rapidocr default)
    constexpr int limit_side = 736;
    const int min_side = std::min(w, h);
    float ratio = 1.0f;
    if (min_side < limit_side) {
      ratio = static_cast<float>(limit_side) / static_cast<float>(min_side);
    }
    int32_t rw = static_cast<int32_t>(std::round(w * ratio));
    int32_t rh = static_cast<int32_t>(std::round(h * ratio));
    rw = std::max(32, static_cast<int32_t>(std::round(rw / 32.0) * 32));
    rh = std::max(32, static_cast<int32_t>(std::round(rh / 32.0) * 32));

    std::vector<uint8_t> resized(static_cast<size_t>(rw * rh * 3));
    resize_rgb_nn(rgb, w, h, stride, resized.data(), rw, rh);
    // Use BGR order for network (PP-OCR convention); convert after resize.
    std::vector<uint8_t> bgr(resized.size());
    rgb24_to_bgr24(resized.data(), bgr.data(), rw, rh, rw * 3, rw * 3);

    std::vector<float> input(static_cast<size_t>(3 * rw * rh));
    rgb_to_nchw_norm(bgr.data(), rw, rh, input.data());  // channel order already BGR packed

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
    // expect NCHW [1,1,H,W] or [1,H,W]
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
    auto db_res = paddle::db_postprocess(out, oh, ow, h, w, opts);

      std::vector<DetBox> boxes;
      boxes.reserve(db_res.aabbs.size());
      for (size_t i = 0; i < db_res.aabbs.size(); ++i) {
        const auto& a = db_res.aabbs[i];
        float score = (i < db_res.quads.size()) ? db_res.quads[i].score : 1.0f;
        boxes.push_back(DetBox{a.x, a.y, a.x + a.width, a.y + a.height, score});
      }
      return boxes;
    }
  }

  paddle_detail::CtcResult run_rec(const uint8_t* rgb, int32_t w, int32_t h, int32_t stride,
                                   const DetBox& box) {
    const int32_t bw = std::max(1, box.x1 - box.x0);
    const int32_t bh = std::max(1, box.y1 - box.y0);
    std::vector<uint8_t> crop(static_cast<size_t>(bw * bh * 3));
    for (int32_t y = 0; y < bh; ++y) {
      const uint8_t* sp = rgb + (box.y0 + y) * stride + box.x0 * 3;
      uint8_t* dp = crop.data() + y * bw * 3;
      std::copy(sp, sp + bw * 3, dp);
    }

    constexpr int32_t rec_h = 48;
    const float wh_ratio = static_cast<float>(bw) / static_cast<float>(std::max(1, bh));
    int32_t rec_w = static_cast<int32_t>(std::ceil(rec_h * wh_ratio));
    rec_w = std::max(rec_w, 8);
    // cap extreme widths
    rec_w = std::min(rec_w, 3200);

    std::vector<uint8_t> resized(static_cast<size_t>(rec_w * rec_h * 3));
    resize_rgb_nn(crop.data(), bw, bh, bw * 3, resized.data(), rec_w, rec_h);
    std::vector<uint8_t> bgr(resized.size());
    rgb24_to_bgr24(resized.data(), bgr.data(), rec_w, rec_h, rec_w * 3, rec_w * 3);

    std::vector<float> input(static_cast<size_t>(3 * rec_w * rec_h));
    rgb_to_nchw_norm(bgr.data(), rec_w, rec_h, input.data());

    std::array<int64_t, 4> shape{1, 3, rec_h, rec_w};
    Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    Ort::Value tensor = Ort::Value::CreateTensor<float>(mem, input.data(), input.size(),
                                                        shape.data(), shape.size());
    const char* in_names[] = {rec_input_name.c_str()};
    const char* out_names[] = {rec_output_name.c_str()};
    auto outputs = rec_session->Run(Ort::RunOptions{nullptr}, in_names, &tensor, 1, out_names, 1);
    float* out = outputs[0].GetTensorMutableData<float>();
    auto dims = outputs[0].GetTensorTypeAndShapeInfo().GetShape();
    // [N, T, C] or [N, C, T] — rapidocr PP-OCRv6 is [N, T, C] with C=18710
    int seq_len = 0;
    int num_classes = 0;
    if (dims.size() == 3) {
      seq_len = static_cast<int>(dims[1]);
      num_classes = static_cast<int>(dims[2]);
      if (num_classes < 10 && dims[1] > dims[2]) {
        // unlikely NCH swapped
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

    auto boxes = impl_->run_det(rgb, w, h, stride);
    if (boxes.empty()) {
      // Empty Det short-circuit: immediately return empty result
      return OcrResult::from_lines({});
    }

    std::vector<OcrLine> lines;
    lines.reserve(boxes.size());
    for (const auto& box : boxes) {
      auto ctc = impl_->run_rec(rgb, w, h, stride, box);
      if (is_strip_empty(ctc.text)) {
        continue;
      }
      OcrCropBox crop{box.x0, box.y0, box.x1 - box.x0, box.y1 - box.y0};
      crop = clamp_box(crop, w, h);
      lines.push_back(OcrLine{
          .text = std::move(ctc.text),
          .confidence = ctc.confidence,
          .box = crop,
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
