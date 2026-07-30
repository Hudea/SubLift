#include "sublift/paddle.hpp"
#include "sublift/diagnostics/paddle_stage_trace.hpp"
#include "sublift/ports/paddle_geometry.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "paddle_models.hpp"
#include "ppocr_crop_cls.hpp"
#include "ppocr_ctc.hpp"
#include "ppocr_det_preprocess.hpp"
#include "ppocr_db_postprocess.hpp"
#include "sublift/hash.hpp"
#include "sublift/image.hpp"
#include "sublift/models/resource_locator.hpp"

#if defined(SUBLIFT_HAS_PADDLE) && SUBLIFT_HAS_PADDLE
#include <onnxruntime_cxx_api.h>

#if defined(SUBLIFT_HAS_OPENCV) || __has_include(<opencv2/opencv.hpp>)
#include <opencv2/opencv.hpp>
#define SUBLIFT_PADDLE_USE_OPENCV 1
#endif
#endif

namespace sublift {

void PaddleStageTrace::clear() {
  *this = PaddleStageTrace{};
}

#if defined(SUBLIFT_HAS_PADDLE) && SUBLIFT_HAS_PADDLE

namespace {

using SteadyClock = std::chrono::steady_clock;

double elapsed_ms(SteadyClock::time_point start) {
  return std::chrono::duration<double, std::milli>(
             SteadyClock::now() - start)
      .count();
}

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

std::vector<std::string> parse_dictionary(std::string_view contents) {
  std::istringstream input{std::string(contents)};
  std::vector<std::string> dictionary;
  std::string line;
  while (std::getline(input, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    dictionary.push_back(line);
  }
  dictionary.emplace_back(" ");
  if (dictionary.size() <= 1) {
    throw std::runtime_error("Paddle Rec 字典为空");
  }
  return dictionary;
}

std::vector<std::string> load_verified_dictionary(
    const std::filesystem::path& keys_path) {
  std::ifstream in(keys_path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("无法读取 Paddle 字典: " + keys_path.string());
  }
  const std::vector<std::uint8_t> bytes{
      std::istreambuf_iterator<char>(in),
      std::istreambuf_iterator<char>()};
  const auto digest = sublift::sha256_prefixed(
      std::span<const std::uint8_t>(bytes));
  static constexpr std::string_view kPpocrV6DictionarySha =
      "sha256:b5f2bfe2bdd9448429e3e82b51c789775d9b42f2403d082b00662eb77e401c5d";
  if (digest != kPpocrV6DictionarySha) {
    throw std::runtime_error(
        "Paddle Rec 模型缺少 character 元数据，且外部字典未通过 SHA-256 "
        "校验: " + keys_path.string() + " (" + digest + ")");
  }
  return parse_dictionary(std::string_view(
      reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

Point2D map_point_to_original(
    Point2D point,
    const paddle::GlobalPreprocessResult& global,
    std::int32_t original_width,
    std::int32_t original_height) {
  point.x = static_cast<float>(
      (point.x - global.padding_left) * global.ratio_w);
  point.y = static_cast<float>(
      (point.y - global.padding_top) * global.ratio_h);
  point.x = std::clamp(point.x, 0.0F, static_cast<float>(original_width));
  point.y = std::clamp(point.y, 0.0F, static_cast<float>(original_height));
  return point;
}

}  // namespace

bool is_paddle_available() noexcept {
  return PaddleOcrEngine::probe_capabilities().available;
}

PaddleCapabilities PaddleOcrEngine::probe_capabilities(
    const std::string& model_root_dir, const std::string& model_type) {
  PaddleCapabilities caps;
  caps.model_type = model_type;
  caps.model_root = model_root_dir;
#if !SUBLIFT_HAS_PADDLE
  caps.available = false;
  caps.detail = "SUBLIFT_ENABLE_PADDLE=OFF";
  return caps;
#else
  try {
    models::ResourceLocator locator;
    auto type = models::parse_model_type(model_type);
    auto res = locator.probe_model_bundle(model_root_dir, type);
    caps.available = res.found;
    if (res.found) {
      caps.model_root = res.value.det_path.parent_path().string();
      caps.detail = "ok";
    } else {
      caps.detail = res.error_msg;
    }
  } catch (const std::exception& e) {
    caps.available = false;
    caps.detail = e.what();
  }
  return caps;
#endif
}

struct PaddleOcrEngine::Impl {
  PaddleOcrOptions options;
  PaddleRuntimeStats runtime_stats;
  paddle_detail::ModelType model_type;
  std::filesystem::path model_dir;
  paddle_detail::ModelPaths model_paths;
  std::vector<std::string> dictionary;

  Ort::Env env;
  Ort::SessionOptions session_options;
  Ort::MemoryInfo memory_info;
  std::unique_ptr<Ort::Session> det_session;
  std::unique_ptr<Ort::Session> cls_session;
  std::unique_ptr<Ort::Session> rec_session;

  std::string det_input_name;
  std::string det_output_name;
  std::string cls_input_name;
  std::string cls_output_name;
  std::string rec_input_name;
  std::string rec_output_name;

  Impl(PaddleOcrOptions opts)
      : options(std::move(opts)),
        model_type(paddle_detail::parse_model_type(options.model_type)),
        env(ORT_LOGGING_LEVEL_WARNING, "sublift_paddle"),
        memory_info(
            Ort::MemoryInfo::CreateCpu(
                OrtArenaAllocator, OrtMemTypeDefault)) {
    models::ResourceLocator locator;
    model_paths = locator.locate_model_bundle(options.model_root_dir, model_type);
    model_dir = model_paths.det_path.parent_path();

    // Match RapidOCR 3.9.2: leave intra/inter thread counts at ORT defaults,
    // disable the CPU arena, and enable all graph optimizations. The former
    // single-thread override both changed reduction numerics and caused the
    // audited Paddle performance regression.
    if (options.intra_op_threads < 0 ||
        options.cls_batch_size <= 0 ||
        options.rec_batch_size <= 0) {
      throw std::invalid_argument(
          "Paddle OCR thread count must be >= 0 and batch sizes must be > 0");
    }
    if (options.intra_op_threads > 0) {
      session_options.SetIntraOpNumThreads(options.intra_op_threads);
    }
    session_options.DisableCpuMemArena();
    session_options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

    det_session = std::make_unique<Ort::Session>(env, model_paths.det_path.c_str(), session_options);
    cls_session = std::make_unique<Ort::Session>(env, model_paths.cls_path.c_str(), session_options);
    rec_session = std::make_unique<Ort::Session>(env, model_paths.rec_path.c_str(), session_options);

    Ort::AllocatorWithDefaultOptions alloc;
    {
      auto in = det_session->GetInputNameAllocated(0, alloc);
      det_input_name = in.get();
      auto out = det_session->GetOutputNameAllocated(0, alloc);
      det_output_name = out.get();
    }
    {
      auto in = cls_session->GetInputNameAllocated(0, alloc);
      cls_input_name = in.get();
      auto out = cls_session->GetOutputNameAllocated(0, alloc);
      cls_output_name = out.get();
    }
    {
      auto in = rec_session->GetInputNameAllocated(0, alloc);
      rec_input_name = in.get();
      auto out = rec_session->GetOutputNameAllocated(0, alloc);
      rec_output_name = out.get();
    }
    try {
      auto metadata = rec_session->GetModelMetadata();
      auto characters =
          metadata.LookupCustomMetadataMapAllocated("character", alloc);
      if (characters && characters.get()[0] != '\0') {
        dictionary = parse_dictionary(characters.get());
      }
    } catch (const Ort::Exception&) {
      // Older/custom exported models may omit metadata. Only a known,
      // cryptographically verified external dictionary may be used below.
    }
    if (dictionary.empty()) {
      dictionary = load_verified_dictionary(resolve_keys_path(model_paths));
    }
  }

  paddle::DBPostProcessResult run_det(
      const paddle::GlobalPreprocessResult& global,
      PaddleStageTrace* trace) {
    const auto preprocess_start = SteadyClock::now();
    auto prepared = paddle::prepare_det_input(
        global.rgb.data(), global.width, global.height);
    const auto rw = prepared.width;
    const auto rh = prepared.height;
    auto& input = prepared.nchw;

    std::array<int64_t, 4> shape{1, 3, rh, rw};
    if (trace != nullptr) {
      trace->det_input.shape.assign(shape.begin(), shape.end());
      trace->det_input.values = input;
      trace->det_preprocess_ms = elapsed_ms(preprocess_start);
      trace->det_call_count += 1;
    }

    const auto infer_start = SteadyClock::now();
    Ort::Value tensor = Ort::Value::CreateTensor<float>(memory_info, input.data(), input.size(),
                                                        shape.data(), shape.size());
    const char* in_names[] = {det_input_name.c_str()};
    const char* out_names[] = {det_output_name.c_str()};
    auto outputs = det_session->Run(Ort::RunOptions{nullptr}, in_names, &tensor, 1, out_names, 1);
    float* out = outputs[0].GetTensorMutableData<float>();
    auto info = outputs[0].GetTensorTypeAndShapeInfo();
    auto dims = info.GetShape();
    const std::size_t output_count = info.GetElementCount();
    if (trace != nullptr) {
      trace->det_probability_map.shape = dims;
      trace->det_probability_map.values.assign(out, out + output_count);
      trace->det_infer_ms = elapsed_ms(infer_start);
    }
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
    opts.det_box_thresh = 0.5F;
    const auto postprocess_start = SteadyClock::now();
    auto result = paddle::db_postprocess(
        out, oh, ow, global.height, global.width, opts);
    paddle::sort_db_result(&result);
    runtime_stats.det_boxes += result.quads.size();
    if (trace != nullptr) {
      trace->det_quads.reserve(result.quads.size());
      for (const auto& quad : result.quads) {
        trace->det_quads.push_back(PaddleQuadTrace{
            .x0 = quad.p0.x,
            .y0 = quad.p0.y,
            .x1 = quad.p1.x,
            .y1 = quad.p1.y,
            .x2 = quad.p2.x,
            .y2 = quad.p2.y,
            .x3 = quad.p3.x,
            .y3 = quad.p3.y,
            .score = quad.score,
        });
      }
      trace->det_postprocess_ms = elapsed_ms(postprocess_start);
    }
    return result;
  }

  std::vector<paddle::ClsResult> run_cls(
      std::vector<paddle::CropResult>& crops,
      PaddleStageTrace* trace) {
    constexpr std::int64_t kChannels = 3;
    constexpr std::int64_t kHeight = 48;
    constexpr std::int64_t kWidth = 192;
    constexpr float kRotateThreshold = 0.9F;

    std::vector<paddle::ClsResult> results(crops.size());
    std::vector<std::size_t> indices(crops.size());
    std::iota(indices.begin(), indices.end(), 0);
    std::stable_sort(
        indices.begin(),
        indices.end(),
        [&](std::size_t left, std::size_t right) {
          const double left_ratio =
              static_cast<double>(crops[left].width) /
              static_cast<double>(std::max(1, crops[left].height));
          const double right_ratio =
              static_cast<double>(crops[right].width) /
              static_cast<double>(std::max(1, crops[right].height));
          return left_ratio < right_ratio;
        });

    const auto batch_size =
        static_cast<std::size_t>(options.cls_batch_size);
    for (std::size_t begin = 0; begin < indices.size(); begin += batch_size) {
      ++runtime_stats.cls_batches;
      const std::size_t end = std::min(indices.size(), begin + batch_size);
      const std::size_t batch = end - begin;
      const auto preprocess_start = SteadyClock::now();
      std::vector<float> input(
          batch * static_cast<std::size_t>(
                      kChannels * kHeight * kWidth),
          0.0F);
      const std::size_t item_size =
          static_cast<std::size_t>(kChannels * kHeight * kWidth);
      for (std::size_t item = 0; item < batch; ++item) {
        paddle::prepare_cls_tensor(
            crops[indices[begin + item]],
            input.data() + item * item_size);
      }
      std::array<std::int64_t, 4> shape{
          static_cast<std::int64_t>(batch),
          kChannels,
          kHeight,
          kWidth,
      };
      if (trace != nullptr) {
        trace->cls_preprocess_ms += elapsed_ms(preprocess_start);
        trace->cls_inputs.push_back(PaddleTensorTrace{
            .shape = std::vector<std::int64_t>(shape.begin(), shape.end()),
            .values = input,
        });
        trace->cls_call_count += 1;
      }

      const auto infer_start = SteadyClock::now();
      Ort::Value tensor = Ort::Value::CreateTensor<float>(
          memory_info,
          input.data(),
          input.size(),
          shape.data(),
          shape.size());
      const char* input_names[] = {cls_input_name.c_str()};
      const char* output_names[] = {cls_output_name.c_str()};
      auto outputs = cls_session->Run(
          Ort::RunOptions{nullptr},
          input_names,
          &tensor,
          1,
          output_names,
          1);
      const float* output = outputs[0].GetTensorData<float>();
      const auto output_info = outputs[0].GetTensorTypeAndShapeInfo();
      const auto output_shape = output_info.GetShape();
      if (output_shape.size() != 2 ||
          output_shape[0] != static_cast<std::int64_t>(batch) ||
          output_shape[1] != 2) {
        throw std::runtime_error("PaddleOCR cls 输出维度异常");
      }
      if (trace != nullptr) {
        trace->cls_infer_ms += elapsed_ms(infer_start);
        trace->cls_logits.push_back(PaddleTensorTrace{
            .shape = output_shape,
            .values = std::vector<float>(
                output, output + output_info.GetElementCount()),
        });
      }

      const auto postprocess_start = SteadyClock::now();
      for (std::size_t item = 0; item < batch; ++item) {
        const float* scores = output + item * 2;
        const int label = scores[1] > scores[0] ? 1 : 0;
        const float score = scores[label];
        const std::size_t original_index = indices[begin + item];
        auto result = paddle::apply_cls_result(
            crops[original_index], label, score, kRotateThreshold);
        results[original_index] = result;
        if (trace != nullptr) {
          trace->cls_results.push_back(PaddleClsResultTrace{
              .label = label == 1 ? "180" : "0",
              .score = score,
              .rotated_180 = result.rotated_180,
          });
        }
      }
      if (trace != nullptr) {
        trace->cls_postprocess_ms += elapsed_ms(postprocess_start);
      }
    }
    return results;
  }

  std::vector<paddle_detail::CtcResult> run_rec(
      const std::vector<paddle::CropResult>& crops,
      PaddleStageTrace* trace) {
    constexpr std::int64_t kChannels = 3;
    constexpr std::int64_t kHeight = 48;
    constexpr double kBaseWidthRatio = 320.0 / 48.0;

    std::vector<paddle_detail::CtcResult> results(crops.size());
    std::vector<std::size_t> indices(crops.size());
    std::iota(indices.begin(), indices.end(), 0);
    std::stable_sort(
        indices.begin(),
        indices.end(),
        [&](std::size_t left, std::size_t right) {
          const double left_ratio =
              static_cast<double>(crops[left].width) /
              static_cast<double>(std::max(1, crops[left].height));
          const double right_ratio =
              static_cast<double>(crops[right].width) /
              static_cast<double>(std::max(1, crops[right].height));
          return left_ratio < right_ratio;
        });

    const auto batch_size =
        static_cast<std::size_t>(options.rec_batch_size);
    for (std::size_t begin = 0; begin < indices.size(); begin += batch_size) {
      ++runtime_stats.rec_batches;
      const std::size_t end = std::min(indices.size(), begin + batch_size);
      const std::size_t batch = end - begin;
      double max_width_ratio = kBaseWidthRatio;
      for (std::size_t item = begin; item < end; ++item) {
        const auto& crop = crops[indices[item]];
        max_width_ratio = std::max(
            max_width_ratio,
            static_cast<double>(crop.width) /
                static_cast<double>(std::max(1, crop.height)));
      }
      const auto target_width = std::max<std::int64_t>(
          1,
          static_cast<std::int64_t>(
              static_cast<double>(kHeight) * max_width_ratio));
      const auto preprocess_start = SteadyClock::now();
      const std::size_t item_size = static_cast<std::size_t>(
          kChannels * kHeight * target_width);
      std::vector<float> input(batch * item_size, 0.0F);
      for (std::size_t item = 0; item < batch; ++item) {
        paddle::prepare_rec_tensor(
            crops[indices[begin + item]],
            static_cast<int>(kHeight),
            static_cast<int>(target_width),
            input.data() + item * item_size);
      }
      std::array<std::int64_t, 4> shape{
          static_cast<std::int64_t>(batch),
          kChannels,
          kHeight,
          target_width,
      };
      if (trace != nullptr) {
        trace->rec_preprocess_ms += elapsed_ms(preprocess_start);
        trace->rec_inputs.push_back(PaddleTensorTrace{
            .shape = std::vector<std::int64_t>(shape.begin(), shape.end()),
            .values = input,
        });
        trace->rec_call_count += 1;
      }

      const auto infer_start = SteadyClock::now();
      Ort::Value tensor = Ort::Value::CreateTensor<float>(
          memory_info,
          input.data(),
          input.size(),
          shape.data(),
          shape.size());
      const char* input_names[] = {rec_input_name.c_str()};
      const char* output_names[] = {rec_output_name.c_str()};
      auto outputs = rec_session->Run(
          Ort::RunOptions{nullptr},
          input_names,
          &tensor,
          1,
          output_names,
          1);
      const float* output = outputs[0].GetTensorData<float>();
      const auto output_info = outputs[0].GetTensorTypeAndShapeInfo();
      const auto output_shape = output_info.GetShape();
      if (output_shape.size() != 3 ||
          output_shape[0] != static_cast<std::int64_t>(batch)) {
        throw std::runtime_error("PaddleOCR rec 输出维度异常");
      }
      const int sequence_length = static_cast<int>(output_shape[1]);
      const int class_count = static_cast<int>(output_shape[2]);
      if (sequence_length <= 0 || class_count <= 1 ||
          static_cast<std::size_t>(class_count - 1) != dictionary.size()) {
        throw std::runtime_error(
            "PaddleOCR rec 输出类别数与模型字典不一致");
      }
      if (trace != nullptr) {
        trace->rec_infer_ms += elapsed_ms(infer_start);
        trace->rec_logits.push_back(PaddleTensorTrace{
            .shape = output_shape,
            .values = std::vector<float>(
                output, output + output_info.GetElementCount()),
        });
      }

      const auto decode_start = SteadyClock::now();
      const std::size_t output_item_size =
          static_cast<std::size_t>(sequence_length) *
          static_cast<std::size_t>(class_count);
      for (std::size_t item = 0; item < batch; ++item) {
        auto decoded = paddle_detail::ctc_greedy_decode(
            output + item * output_item_size,
            sequence_length,
            class_count,
            dictionary);
        results[indices[begin + item]] = decoded;
        if (trace != nullptr) {
          trace->decoded_results.push_back(PaddleDecodedTrace{
              .ctc_tokens = decoded.token_indices,
              .decoded_text = decoded.text,
              .decoded_confidence = decoded.confidence,
          });
        }
      }
      if (trace != nullptr) {
        trace->rec_decode_ms += elapsed_ms(decode_start);
      }
    }
    return results;
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
  if (!impl_ || !impl_->det_session ||
      !impl_->cls_session || !impl_->rec_session) {
    throw std::runtime_error("PaddleOCR 推理会话未初始化");
  }

  try {
    ++impl_->runtime_stats.recognize_calls;
    const uint8_t* rgb = image.data();
    const int32_t w = image.width();
    const int32_t h = image.height();
    const int32_t stride = static_cast<int32_t>(image.stride_bytes());
    PaddleStageTrace* trace =
        impl_->options.dump_stages ? impl_->options.stage_trace : nullptr;
    if (trace != nullptr) {
      trace->clear();
      trace->recognize_call_count = 1;
      trace->input_width = w;
      trace->input_height = h;
      trace->input_stride = stride;
      trace->input_color_order = "RGB";
      trace->cls_enabled = true;
      trace->input_rgb.resize(
          static_cast<std::size_t>(w) * static_cast<std::size_t>(h) * 3);
      for (std::int32_t y = 0; y < h; ++y) {
        std::memcpy(
            trace->input_rgb.data() +
                static_cast<std::size_t>(y) * static_cast<std::size_t>(w) * 3,
            rgb + static_cast<std::size_t>(y) *
                      static_cast<std::size_t>(stride),
            static_cast<std::size_t>(w) * 3);
      }
    }

    const auto global_start = SteadyClock::now();
    const auto global = paddle::prepare_global_image(rgb, w, h, stride);
    if (trace != nullptr) {
      trace->working_width = global.width;
      trace->working_height = global.height;
      trace->working_rgb = global.rgb;
      trace->global_ratio_h = global.ratio_h;
      trace->global_ratio_w = global.ratio_w;
      trace->global_padding_top = global.padding_top;
      trace->global_padding_left = global.padding_left;
      trace->global_preprocess_ms = elapsed_ms(global_start);
    }

    auto db_res = impl_->run_det(global, trace);
    if (impl_->options.det_quads_override != nullptr) {
      db_res.quads.clear();
      db_res.quads.reserve(impl_->options.det_quads_override->size());
      for (const auto& frozen : *impl_->options.det_quads_override) {
        db_res.quads.push_back(paddle::QuadPolygon{
            .p0 = Point2D{frozen.x0, frozen.y0},
            .p1 = Point2D{frozen.x1, frozen.y1},
            .p2 = Point2D{frozen.x2, frozen.y2},
            .p3 = Point2D{frozen.x3, frozen.y3},
            .score = frozen.score,
        });
      }
      if (trace != nullptr) {
        trace->det_quads = *impl_->options.det_quads_override;
      }
    }
    if (db_res.quads.empty()) {
      if (trace != nullptr) {
        trace->output_lines.clear();
      }
      return OcrResult::from_lines({});
    }

    std::vector<paddle::CropResult> crops;
    crops.reserve(db_res.quads.size());
    for (size_t i = 0; i < db_res.quads.size(); ++i) {
      const auto& quad = db_res.quads[i];

      const auto crop_start = SteadyClock::now();
      paddle::CropResult crop = paddle::get_rotate_crop(
          global.rgb.data(), global.width, global.height, global.width * 3,
          quad);
      if (trace != nullptr) {
        trace->crop_ms += elapsed_ms(crop_start);
        trace->crops.push_back(PaddleCropStageTrace{
            .width = crop.width,
            .height = crop.height,
            .rotated_90 = crop.rotated_90,
            .rgb = crop.rgb_data,
        });
      }
      crops.push_back(std::move(crop));
    }

    (void)impl_->run_cls(crops, trace);
    auto recognized = impl_->run_rec(crops, trace);

    std::vector<OcrLine> lines;
    lines.reserve(db_res.quads.size());
    for (std::size_t i = 0; i < db_res.quads.size(); ++i) {
      const auto& quad = db_res.quads[i];
      auto& ctc = recognized[i];
      if (is_strip_empty(ctc.text) || ctc.confidence < 0.5f) {
        continue;
      }
      const std::array<Point2D, 4> original_points{
          map_point_to_original(quad.p0, global, w, h),
          map_point_to_original(quad.p1, global, w, h),
          map_point_to_original(quad.p2, global, w, h),
          map_point_to_original(quad.p3, global, w, h),
      };
      OcrCropBox crop_box = clamp_box(quad_to_aabb(original_points), w, h);
      lines.push_back(OcrLine{
          .text = std::move(ctc.text),
          .confidence = ctc.confidence,
          .box = crop_box,
      });
    }
    const auto output_start = SteadyClock::now();
    sort_ocr_lines(lines);
    OcrResult result = OcrResult::from_lines(lines);
    if (trace != nullptr) {
      trace->output_lines = result.lines;
      trace->output_ms = elapsed_ms(output_start);
    }
    return result;
  } catch (const Ort::Exception& e) {
    throw std::runtime_error(std::string("PaddleOCR ONNX Runtime 推理故障: ") + e.what());
  } catch (const std::runtime_error&) {
    throw;
  } catch (const std::exception& e) {
    throw std::runtime_error(std::string("PaddleOCR 推理故障: ") + e.what());
  }
}

PaddleRuntimeStats PaddleOcrEngine::runtime_stats() const noexcept {
  return impl_ ? impl_->runtime_stats : PaddleRuntimeStats{};
}

#else

bool is_paddle_available() noexcept { return false; }

PaddleCapabilities PaddleOcrEngine::probe_capabilities(
    const std::string& model_root_dir, const std::string& model_type) {
  PaddleCapabilities caps;
  caps.available = false;
  caps.model_type = model_type;
  caps.model_root = model_root_dir;
  caps.detail = "SUBLIFT_ENABLE_PADDLE=OFF";
  return caps;
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

PaddleRuntimeStats PaddleOcrEngine::runtime_stats() const noexcept {
  return {};
}

#endif

}  // namespace sublift
