#include <bit>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "paddle_models.hpp"
#include "sublift/image.hpp"
#include "sublift/paddle.hpp"
#include "sublift/test_support.hpp"

namespace {

using json = nlohmann::json;

struct Args {
  std::filesystem::path rgb_path;
  std::filesystem::path output_path;
  std::filesystem::path raw_dir;
  std::filesystem::path quads_json_path;
  std::string model_root;
  std::string model_type{"small"};
  std::int32_t width{0};
  std::int32_t height{0};
  std::int32_t intra_op_threads{0};
  std::int32_t cls_batch_size{6};
  std::int32_t rec_batch_size{6};
};

[[noreturn]] void usage_error(std::string_view message) {
  throw std::invalid_argument(
      std::string(message) +
      "\nUsage: sublift_paddle_trace --rgb <RGB24.raw> --width <px> "
      "--height <px> --out <trace.json> [--raw-dir <dir>] "
      "[--quads-json <frozen-quads.json>] [--model-root <dir>] "
      "[--model-type tiny|small|medium] [--intra-op-threads <n>] "
      "[--cls-batch-size <n>] [--rec-batch-size <n>]");
}

std::int32_t parse_positive_i32(std::string_view value, std::string_view name) {
  std::size_t consumed = 0;
  long parsed = 0;
  try {
    parsed = std::stol(std::string(value), &consumed, 10);
  } catch (const std::exception&) {
    usage_error(std::string(name) + " must be a positive integer");
  }
  if (consumed != value.size() || parsed <= 0 ||
      parsed > std::numeric_limits<std::int32_t>::max()) {
    usage_error(std::string(name) + " must be a positive int32");
  }
  return static_cast<std::int32_t>(parsed);
}

Args parse_args(int argc, char* argv[]) {
  Args args;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    auto require_value = [&](std::string_view option) -> std::string_view {
      if (i + 1 >= argc) {
        usage_error(std::string(option) + " requires a value");
      }
      return argv[++i];
    };

    if (arg == "--rgb") {
      args.rgb_path = require_value(arg);
    } else if (arg == "--width") {
      args.width = parse_positive_i32(require_value(arg), arg);
    } else if (arg == "--height") {
      args.height = parse_positive_i32(require_value(arg), arg);
    } else if (arg == "--out") {
      args.output_path = require_value(arg);
    } else if (arg == "--raw-dir") {
      args.raw_dir = require_value(arg);
    } else if (arg == "--quads-json") {
      args.quads_json_path = require_value(arg);
    } else if (arg == "--model-root") {
      args.model_root = std::string(require_value(arg));
    } else if (arg == "--model-type") {
      args.model_type = std::string(require_value(arg));
    } else if (arg == "--intra-op-threads") {
      args.intra_op_threads = parse_positive_i32(require_value(arg), arg);
    } else if (arg == "--cls-batch-size") {
      args.cls_batch_size = parse_positive_i32(require_value(arg), arg);
    } else if (arg == "--rec-batch-size") {
      args.rec_batch_size = parse_positive_i32(require_value(arg), arg);
    } else {
      usage_error("unknown option: " + std::string(arg));
    }
  }

  if (args.rgb_path.empty() || args.output_path.empty() ||
      args.width <= 0 || args.height <= 0) {
    usage_error("--rgb, --width, --height and --out are required");
  }
  return args;
}

std::vector<std::uint8_t> read_bytes(const std::filesystem::path& path) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) {
    throw std::runtime_error("cannot open input: " + path.string());
  }
  return std::vector<std::uint8_t>(
      std::istreambuf_iterator<char>(stream),
      std::istreambuf_iterator<char>());
}

std::vector<sublift::PaddleQuadTrace> read_frozen_quads(
    const std::filesystem::path& path) {
  std::ifstream stream(path);
  if (!stream) {
    throw std::runtime_error("cannot open frozen quads: " + path.string());
  }
  const auto document = json::parse(stream);
  if (!document.is_array()) {
    throw std::runtime_error("frozen quads JSON must be an array");
  }
  std::vector<sublift::PaddleQuadTrace> result;
  result.reserve(document.size());
  for (const auto& item : document) {
    const auto& points = item.at("points");
    if (!points.is_array() || points.size() != 4) {
      throw std::runtime_error(
          "each frozen quad must contain four points");
    }
    result.push_back(sublift::PaddleQuadTrace{
        .x0 = points[0][0].get<float>(),
        .y0 = points[0][1].get<float>(),
        .x1 = points[1][0].get<float>(),
        .y1 = points[1][1].get<float>(),
        .x2 = points[2][0].get<float>(),
        .y2 = points[2][1].get<float>(),
        .x3 = points[3][0].get<float>(),
        .y3 = points[3][1].get<float>(),
        .score = item.value("score", 1.0F),
    });
  }
  return result;
}

std::string sha256_bytes(const void* data, std::size_t size) {
  static constexpr std::uint8_t kEmpty = 0;
  const auto* bytes =
      size == 0 ? &kEmpty : static_cast<const std::uint8_t*>(data);
  return sublift::test_support::sha256_prefixed(
      bytes, size);
}

std::string sha256_file(const std::filesystem::path& path) {
  const auto bytes = read_bytes(path);
  return sha256_bytes(bytes.data(), bytes.size());
}

void write_bytes(
    const std::filesystem::path& path,
    const void* data,
    std::size_t size) {
  std::filesystem::create_directories(path.parent_path());
  std::ofstream stream(path, std::ios::binary | std::ios::trunc);
  if (!stream) {
    throw std::runtime_error("cannot write raw trace: " + path.string());
  }
  stream.write(static_cast<const char*>(data), static_cast<std::streamsize>(size));
  if (!stream) {
    throw std::runtime_error("failed writing raw trace: " + path.string());
  }
}

template <typename T>
json numeric_summary(
    const std::vector<T>& values,
    std::string_view dtype,
    const std::filesystem::path& raw_dir,
    std::string_view raw_name) {
  json result = {
      {"dtype", dtype},
      {"count", values.size()},
      {"sha256", sha256_bytes(values.data(), values.size() * sizeof(T))},
  };

  if (values.empty()) {
    result["min"] = nullptr;
    result["max"] = nullptr;
    result["mean"] = nullptr;
  } else {
    double min_value = static_cast<double>(values.front());
    double max_value = min_value;
    long double sum = 0.0;
    for (const auto value : values) {
      const double converted = static_cast<double>(value);
      min_value = std::min(min_value, converted);
      max_value = std::max(max_value, converted);
      sum += static_cast<long double>(converted);
    }
    result["min"] = min_value;
    result["max"] = max_value;
    result["mean"] =
        static_cast<double>(sum / static_cast<long double>(values.size()));
  }

  if (!raw_dir.empty()) {
    const auto filename = std::string(raw_name) + ".bin";
    write_bytes(
        raw_dir / filename, values.data(), values.size() * sizeof(T));
    result["raw_file"] = filename;
  }
  return result;
}

json tensor_summary(
    const sublift::PaddleTensorTrace& tensor,
    const std::filesystem::path& raw_dir,
    std::string_view raw_name) {
  json result = numeric_summary(
      tensor.values, "float32-le", raw_dir, raw_name);
  result["shape"] = tensor.shape;
  return result;
}

json image_summary(
    const std::vector<std::uint8_t>& rgb,
    std::int32_t width,
    std::int32_t height,
    const std::filesystem::path& raw_dir,
    std::string_view raw_name) {
  json result = numeric_summary(rgb, "uint8", raw_dir, raw_name);
  result["shape"] = {height, width, 3};
  result["color_order"] = "RGB";
  return result;
}

json line_json(const sublift::OcrLine& line) {
  return {
      {"text", line.text},
      {"confidence", line.confidence},
      {"box",
       {
           {"x", line.box.x},
           {"y", line.box.y},
           {"width", line.box.width},
           {"height", line.box.height},
       }},
  };
}

json trace_json(
    const sublift::PaddleStageTrace& trace,
    const sublift::OcrResult& result,
    const std::filesystem::path& raw_dir) {
  json quads = json::array();
  for (const auto& quad : trace.det_quads) {
    quads.push_back({
        {"points",
         {
             {quad.x0, quad.y0},
             {quad.x1, quad.y1},
             {quad.x2, quad.y2},
             {quad.x3, quad.y3},
         }},
        {"score", quad.score},
    });
  }

  json crops = json::array();
  json rec_inputs = json::array();
  json rec_logits = json::array();
  json decoded = json::array();
  for (std::size_t i = 0; i < trace.crops.size(); ++i) {
    const auto& crop = trace.crops[i];
    const auto index = std::to_string(i);
    crops.push_back({
        {"image",
         image_summary(
             crop.rgb, crop.width, crop.height, raw_dir,
             "crop_" + index + "_rgb")},
        {"rotated_90", crop.rotated_90},
    });
  }
  json cls_inputs = json::array();
  json cls_logits = json::array();
  json cls_results = json::array();
  for (std::size_t i = 0; i < trace.cls_inputs.size(); ++i) {
    const auto index = std::to_string(i);
    cls_inputs.push_back(tensor_summary(
        trace.cls_inputs[i], raw_dir, "cls_input_" + index));
  }
  for (std::size_t i = 0; i < trace.cls_logits.size(); ++i) {
    const auto index = std::to_string(i);
    cls_logits.push_back(tensor_summary(
        trace.cls_logits[i], raw_dir, "cls_logits_" + index));
  }
  for (const auto& result : trace.cls_results) {
    cls_results.push_back({
        {"label", result.label},
        {"score", result.score},
        {"rotated_180", result.rotated_180},
    });
  }
  for (std::size_t i = 0; i < trace.rec_inputs.size(); ++i) {
    const auto index = std::to_string(i);
    rec_inputs.push_back(tensor_summary(
        trace.rec_inputs[i], raw_dir, "rec_input_" + index));
  }
  for (std::size_t i = 0; i < trace.rec_logits.size(); ++i) {
    const auto index = std::to_string(i);
    rec_logits.push_back(tensor_summary(
        trace.rec_logits[i], raw_dir, "rec_logits_" + index));
  }
  for (const auto& result : trace.decoded_results) {
    decoded.push_back({
        {"tokens", result.ctc_tokens},
        {"text", result.decoded_text},
        {"confidence", result.decoded_confidence},
    });
  }

  json output_lines = json::array();
  for (const auto& line : result.lines) {
    output_lines.push_back(line_json(line));
  }

  return {
      {"schema_version", trace.schema_version},
      {"runtime", "cpp"},
      {"stages",
       {
           {"1_global_preprocess",
            {
                {"source_image",
                 image_summary(
                     trace.input_rgb, trace.input_width, trace.input_height,
                     raw_dir, "input_rgb")},
                {"working_image",
                 image_summary(
                     trace.working_rgb, trace.working_width,
                     trace.working_height,
                     raw_dir, "working_rgb")},
                {"operations",
                 {
                     {"preprocess",
                      {
                          {"ratio_h", trace.global_ratio_h},
                          {"ratio_w", trace.global_ratio_w},
                      }},
                     {"padding_1",
                      {
                          {"top", trace.global_padding_top},
                          {"left", trace.global_padding_left},
                      }},
                 }},
                {"stride", trace.input_stride},
            }},
           {"2_det_preprocess",
            {{"tensor", tensor_summary(
                            trace.det_input, raw_dir, "det_input")}}},
           {"3_det_infer",
            {{"probability_map",
              tensor_summary(
                  trace.det_probability_map, raw_dir, "det_probability")}}},
           {"4_det_postprocess",
            {{"box_count", quads.size()}, {"quads", std::move(quads)}}},
           {"5_perspective_crop",
            {{"crop_count", crops.size()}, {"crops", std::move(crops)}}},
           {"6_cls",
            {
                {"enabled", trace.cls_enabled},
                {"batch_count", cls_inputs.size()},
                {"tensors", std::move(cls_inputs)},
                {"logits", std::move(cls_logits)},
                {"results", std::move(cls_results)},
            }},
           {"7_rec_preprocess",
            {{"batch_count", rec_inputs.size()},
             {"tensors", std::move(rec_inputs)}}},
           {"8_rec_infer",
            {{"batch_count", rec_logits.size()},
             {"logits", std::move(rec_logits)}}},
           {"9_rec_decode",
            {{"decoded_count", decoded.size()},
             {"results", std::move(decoded)}}},
           {"10_output",
            {
                {"text", result.text},
                {"confidence", result.confidence},
                {"line_count", output_lines.size()},
                {"lines", std::move(output_lines)},
            }},
       }},
      {"counts",
       {
           {"recognize_calls", trace.recognize_call_count},
           {"det_calls", trace.det_call_count},
           {"det_boxes", trace.det_quads.size()},
           {"cls_calls", trace.cls_call_count},
           {"rec_calls", trace.rec_call_count},
       }},
      {"timing_ms",
       {
           {"global_preprocess", trace.global_preprocess_ms},
           {"det_preprocess", trace.det_preprocess_ms},
           {"det_infer", trace.det_infer_ms},
           {"det_postprocess", trace.det_postprocess_ms},
           {"crop", trace.crop_ms},
           {"cls_preprocess", trace.cls_preprocess_ms},
           {"cls_infer", trace.cls_infer_ms},
           {"cls_postprocess", trace.cls_postprocess_ms},
           {"rec_preprocess", trace.rec_preprocess_ms},
           {"rec_infer", trace.rec_infer_ms},
           {"rec_decode", trace.rec_decode_ms},
           {"output", trace.output_ms},
       }},
  };
}

}  // namespace

int main(int argc, char* argv[]) {
  try {
    static_assert(
        std::endian::native == std::endian::little,
        "Paddle trace canonical float encoding requires little-endian");
    const Args args = parse_args(argc, argv);
    const auto input = read_bytes(args.rgb_path);
    const std::size_t expected_size =
        static_cast<std::size_t>(args.width) *
        static_cast<std::size_t>(args.height) * 3;
    if (input.size() != expected_size) {
      throw std::runtime_error(
          "RGB24 byte count mismatch: expected " +
          std::to_string(expected_size) + ", got " +
          std::to_string(input.size()));
    }

    sublift::ImageBuffer image(
        args.width, args.height, sublift::PixelFormat::RGB24);
    std::memcpy(image.data(), input.data(), input.size());

    sublift::PaddleStageTrace trace;
    sublift::PaddleOcrOptions options;
    options.model_type = args.model_type;
    options.model_root_dir = args.model_root;
    options.intra_op_threads = args.intra_op_threads;
    options.cls_batch_size = args.cls_batch_size;
    options.rec_batch_size = args.rec_batch_size;
    options.dump_stages = true;
    options.stage_trace = &trace;
    std::vector<sublift::PaddleQuadTrace> frozen_quads;
    if (!args.quads_json_path.empty()) {
      frozen_quads = read_frozen_quads(args.quads_json_path);
      options.det_quads_override = &frozen_quads;
    }

    sublift::PaddleOcrEngine engine(options);
    const auto result = engine.recognize(image.view());

    const auto model_type =
        sublift::paddle_detail::parse_model_type(args.model_type);
    const auto model_dir =
        sublift::paddle_detail::resolve_model_dir(args.model_root);
    const auto paths =
        sublift::paddle_detail::get_expected_model_paths(model_dir, model_type);
    auto keys_path = paths.keys_path;
    if (!std::filesystem::exists(keys_path)) {
      keys_path = model_dir / "ppocrv6_dict.txt";
    }

    json output = trace_json(trace, result, args.raw_dir);
    output["model"] = {
        {"type", args.model_type},
        {"det_file", paths.det_path.filename().string()},
        {"det_sha256", sha256_file(paths.det_path)},
        {"cls_file", paths.cls_path.filename().string()},
        {"cls_sha256", sha256_file(paths.cls_path)},
        {"rec_file", paths.rec_path.filename().string()},
        {"rec_sha256", sha256_file(paths.rec_path)},
        {"dictionary_file", keys_path.filename().string()},
        {"dictionary_sha256", sha256_file(keys_path)},
    };

    std::filesystem::create_directories(args.output_path.parent_path());
    std::ofstream stream(args.output_path, std::ios::trunc);
    if (!stream) {
      throw std::runtime_error(
          "cannot write trace JSON: " + args.output_path.string());
    }
    stream << output.dump(2) << '\n';
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "sublift_paddle_trace: " << error.what() << '\n';
    return 1;
  }
}
