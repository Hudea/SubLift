#include <sublift/adapters/ffmpeg/ffmpeg_frame_source.hpp>
#include "display_transform_internal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace sublift::adapters::ffmpeg {

namespace {

std::string format_fps(double fps) {
  if (std::isnan(fps) || std::isinf(fps)) {
    std::ostringstream oss;
    oss << fps;
    return oss.str();
  }
  std::ostringstream oss;
  oss << fps;
  std::string s = oss.str();
  if (s.find('.') == std::string::npos && s.find('e') == std::string::npos &&
      s.find('E') == std::string::npos) {
    s += ".0";
  }
  return s;
}

std::string to_lower(std::string_view str) {
  std::string result{str};
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return result;
}

std::string trim(std::string_view str) {
  size_t start = str.find_first_not_of(" \t\n\r");
  if (start == std::string::npos) return "";
  size_t end = str.find_last_not_of(" \t\n\r");
  return std::string{str.substr(start, end - start + 1)};
}

}  // namespace

void validate_output_crop(const SourceBox& crop, std::int32_t source_width,
                          std::int32_t source_height) {
  const auto x = crop.x;
  const auto y = crop.y;
  const auto w = crop.width;
  const auto h = crop.height;

  const std::string requested = "[" + std::to_string(x) + ", " + std::to_string(y) +
                                ", " + std::to_string(w) + ", " + std::to_string(h) +
                                "]";
  const std::string source =
      std::to_string(source_width) + "x" + std::to_string(source_height);

  if (x < 0 || y < 0) {
    throw std::invalid_argument("output_crop 的 x/y 不能为负：source=" + source +
                                " requested=" + requested);
  }
  if (w <= 0 || h <= 0) {
    throw std::invalid_argument("output_crop 的 width/height 必须为正：source=" +
                                source + " requested=" + requested);
  }
  if (static_cast<std::int64_t>(x) + static_cast<std::int64_t>(w) >
          static_cast<std::int64_t>(source_width) ||
      static_cast<std::int64_t>(y) + static_cast<std::int64_t>(h) >
          static_cast<std::int64_t>(source_height)) {
    throw std::invalid_argument("output_crop 越界：source=" + source +
                                " requested=" + requested);
  }
}

TwoStageCropPlan plan_two_stage_crop(
    const SourceBox& crop,
    std::int32_t source_width,
    std::int32_t source_height,
    std::int32_t margin) {
  validate_output_crop(crop, source_width, source_height);
  const std::int64_t m = std::max<std::int64_t>(0, margin);

  // Vertical coarse bounds: expand by margin and align to even rows so 4:2:0
  // chroma phase and multi-tap vertical filter support (8-bit and 10-bit)
  // match full-frame conversion.
  std::int32_t y_coarse = 0;
  std::int32_t h_coarse = source_height;
  if (source_height % 2 == 0) {
    const std::int32_t y1 = static_cast<std::int32_t>(
        std::max<std::int64_t>(0, static_cast<std::int64_t>(crop.y) - m));
    y_coarse = y1 - (y1 % 2);

    const std::int32_t y2 = static_cast<std::int32_t>(std::min<std::int64_t>(
        source_height,
        static_cast<std::int64_t>(crop.y) + crop.height + m));
    const std::int32_t y2_even = (y2 % 2 != 0) ? (y2 + 1) : y2;
    h_coarse = y2_even - y_coarse;
  }

  // Horizontal coarse bounds: libswscale gates its fast SIMD YUV->RGB24 paths
  // on 32-pixel (AVX2) or 16-pixel (NEON/SSE2) width alignment. Align x down
  // and x2 up to 32 when source_width % 32 == 0, or 16 when source_width % 16 == 0,
  // preserving horizontal chroma filter context. When source_width is not a
  // multiple of 16 (e.g. 854x480, 1080x1920), keep full source width so swscale
  // takes the identical horizontal codepath as full-frame conversion.
  std::int32_t x_coarse = 0;
  std::int32_t w_coarse = source_width;
  const std::int64_t h_align =
      (source_width >= 32 && source_width % 32 == 0)
          ? 32
          : ((source_width >= 16 && source_width % 16 == 0) ? 16 : 0);
  if (h_align > 0) {
    const std::int64_t x1 =
        std::max<std::int64_t>(0, static_cast<std::int64_t>(crop.x) - m);
    x_coarse = static_cast<std::int32_t>((x1 / h_align) * h_align);

    const std::int64_t x2 = std::min<std::int64_t>(
        source_width,
        static_cast<std::int64_t>(crop.x) + crop.width + m);
    const std::int64_t x2_aligned = ((x2 + h_align - 1) / h_align) * h_align;
    w_coarse = static_cast<std::int32_t>(x2_aligned) - x_coarse;
  }

  const std::int32_t dx = crop.x - x_coarse;
  const std::int32_t dy = crop.y - y_coarse;

  return TwoStageCropPlan{
      .coarse_crop = SourceBox{x_coarse, y_coarse, w_coarse, h_coarse},
      .exact_crop = SourceBox{dx, dy, crop.width, crop.height},
  };
}

std::string build_optimized_output_vf(
    double fps,
    const std::optional<SourceBox>& crop,
    std::int32_t source_width,
    std::int32_t source_height,
    std::int32_t margin) {
  if (!crop.has_value() || source_width <= 0 || source_height <= 0) {
    return build_output_vf(fps, crop);
  }
  validate_output_crop(*crop, source_width, source_height);
  if (source_width % 2 != 0 || source_height % 2 != 0) {
    return build_output_vf(fps, crop);
  }

  const auto plan = plan_two_stage_crop(*crop, source_width, source_height, margin);
  if (plan.coarse_crop.x == 0 && plan.coarse_crop.y == 0 &&
      plan.coarse_crop.width == source_width &&
      plan.coarse_crop.height == source_height) {
    return build_output_vf(fps, crop);
  }

  const std::string fps_str = format_fps(fps);
  return "fps=" + fps_str +
         ",crop=" + std::to_string(plan.coarse_crop.width) + ":" +
         std::to_string(plan.coarse_crop.height) + ":" +
         std::to_string(plan.coarse_crop.x) + ":" +
         std::to_string(plan.coarse_crop.y) + ":exact=1" +
         ",format=rgb24" +
         ",crop=" + std::to_string(plan.exact_crop.width) + ":" +
         std::to_string(plan.exact_crop.height) + ":" +
         std::to_string(plan.exact_crop.x) + ":" +
         std::to_string(plan.exact_crop.y) + ":exact=1";
}

std::string build_output_vf(double fps, const SourceBox* crop) {
  const std::string fps_str = format_fps(fps);
  if (crop == nullptr) {
    return "fps=" + fps_str;
  }
  return "fps=" + fps_str +
         ",format=rgb24,crop=" + std::to_string(crop->width) + ":" +
         std::to_string(crop->height) + ":" + std::to_string(crop->x) + ":" +
         std::to_string(crop->y) + ":exact=1";
}

std::string build_output_vf(double fps, const std::optional<SourceBox>& crop) {
  return build_output_vf(fps, crop.has_value() ? &crop.value() : nullptr);
}

}  // namespace sublift::adapters::ffmpeg

namespace sublift::adapters::ffmpeg::detail {

std::pair<bool, std::optional<std::string>> assess_display_transform(
    const nlohmann::json& stream_json) {
  if (stream_json.contains("tags") && stream_json["tags"].is_object()) {
    const auto& tags = stream_json["tags"];
    if (tags.contains("rotate")) {
      std::string rotate_str;
      if (tags["rotate"].is_string()) {
        rotate_str = tags["rotate"].get<std::string>();
      } else if (tags["rotate"].is_number()) {
        rotate_str = tags["rotate"].dump();
      }
      rotate_str = trim(rotate_str);
      if (!rotate_str.empty() && rotate_str != "0") {
        std::string note = "stream.tags.rotate=";
        if (tags["rotate"].is_string()) {
          note += "'" + tags["rotate"].get<std::string>() + "'";
        } else {
          note += rotate_str;
        }
        return {false, note};
      }
    }
  }

  if (!stream_json.contains("side_data_list") || stream_json["side_data_list"].is_null()) {
    return {true, std::nullopt};
  }

  const auto& side_data = stream_json["side_data_list"];
  if (!side_data.is_array()) {
    return {false, "stream.side_data_list 不可解析"};
  }

  for (const auto& item : side_data) {
    if (!item.is_object()) {
      return {false, "stream.side_data_list 项不可解析"};
    }
    std::string side_type;
    if (item.contains("side_data_type")) {
      if (item["side_data_type"].is_string()) {
        side_type = item["side_data_type"].get<std::string>();
      } else {
        side_type = item["side_data_type"].dump();
      }
    }
    const std::string lower_type = to_lower(side_type);
    if (lower_type.find("display matrix") == std::string::npos &&
        lower_type.find("displaymatrix") == std::string::npos) {
      continue;
    }

    if (is_identity_display_matrix(item)) {
      continue;
    }
    return {false, "non-identity display matrix: " + side_type};
  }

  return {true, std::nullopt};
}

bool is_identity_display_matrix(const nlohmann::json& side_data_json) {
  if (!side_data_json.is_object()) {
    return false;
  }

  if (side_data_json.contains("rotation") && !side_data_json["rotation"].is_null()) {
    try {
      double rot = 0.0;
      if (side_data_json["rotation"].is_number()) {
        rot = side_data_json["rotation"].get<double>();
      } else if (side_data_json["rotation"].is_string()) {
        rot = std::stod(side_data_json["rotation"].get<std::string>());
      } else {
        return false;
      }
      return std::abs(rot) <= 1e-3;
    } catch (...) {
      return false;
    }
  }

  const nlohmann::json* matrix_ptr = nullptr;
  if (side_data_json.contains("displaymatrix") &&
      !side_data_json["displaymatrix"].is_null()) {
    matrix_ptr = &side_data_json["displaymatrix"];
  } else if (side_data_json.contains("matrix") &&
              !side_data_json["matrix"].is_null()) {
    matrix_ptr = &side_data_json["matrix"];
  }

  if (matrix_ptr == nullptr || matrix_ptr->is_null()) {
    return false;
  }

  std::vector<double> values;
  if (matrix_ptr->is_string()) {
    std::string s = matrix_ptr->get<std::string>();
    std::replace(s.begin(), s.end(), '\n', ' ');
    std::istringstream iss(s);
    double val = 0.0;
    while (iss >> val) {
      values.push_back(val);
    }
  } else if (matrix_ptr->is_array()) {
    for (const auto& el : *matrix_ptr) {
      if (el.is_number()) {
        values.push_back(el.get<double>());
      } else if (el.is_string()) {
        try {
          values.push_back(std::stod(el.get<std::string>()));
        } catch (...) {
          return false;
        }
      } else {
        return false;
      }
    }
  } else {
    return false;
  }

  if (values.size() < 9) {
    return false;
  }

  constexpr double float_id[9] = {1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0};
  bool is_float_identity = true;
  for (int i = 0; i < 9; ++i) {
    if (std::abs(values[i] - float_id[i]) > 1e-3) {
      is_float_identity = false;
      break;
    }
  }
  if (is_float_identity) {
    return true;
  }

  constexpr double fixed_id[9] = {65536.0, 0.0, 0.0, 0.0, 65536.0, 0.0, 0.0, 0.0, 1073741824.0};
  double max_abs = 0.0;
  for (int i = 0; i < 9; ++i) {
    max_abs = std::max(max_abs, std::abs(values[i]));
  }

  if (max_abs > 16.0) {
    for (int i = 0; i < 9; ++i) {
      if (std::abs(values[i] - fixed_id[i]) > 1.0) {
        return false;
      }
    }
    return true;
  }

  return false;
}

}  // namespace sublift::adapters::ffmpeg::detail

namespace sublift::adapters::ffmpeg {

bool is_high_bit_depth_pix_fmt(std::string_view pix_fmt) {
  if (pix_fmt.empty()) {
    return false;
  }
  const std::string lower = to_lower(std::string(pix_fmt));
  static constexpr std::string_view kHighBitMarkers[] = {
      "10le", "10be", "12le", "12be", "14le", "14be", "16le", "16be",
      "p010", "p210", "p410", "p012", "p016", "rgb48", "bgr48", "rgba64", "bgra64",
  };
  for (std::string_view marker : kHighBitMarkers) {
    if (lower.find(marker) != std::string_view::npos) {
      return true;
    }
  }
  return false;
}

std::string resolve_effective_hwaccel(std::string_view requested_hwaccel,
                                      const StreamDecodeHints& hints) {
  std::string trimmed = to_lower(std::string(requested_hwaccel));
  const auto first = trimmed.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return "";
  }
  const auto last = trimmed.find_last_not_of(" \t\r\n");
  trimmed = trimmed.substr(first, last - first + 1);

  if (trimmed.empty() || trimmed == "none" || trimmed == "off" ||
      trimmed == "false" || trimmed == "0") {
    return "";
  }
  if (trimmed != "auto") {
    std::string raw(requested_hwaccel);
    const auto r_first = raw.find_first_not_of(" \t\r\n");
    const auto r_last = raw.find_last_not_of(" \t\r\n");
    return raw.substr(r_first, r_last - r_first + 1);
  }

#if defined(__APPLE__)
  constexpr std::string_view kNativeHwaccel = "videotoolbox";
#else
  constexpr std::string_view kNativeHwaccel = "";
#endif
  if (kNativeHwaccel.empty()) {
    return "";
  }

  const std::string codec = to_lower(hints.codec_name);
  const bool heavy_codec =
      (codec == "hevc" || codec == "h265" || codec == "av1" ||
       codec == "libdav1d" || codec == "vp9" || codec == "prores");
  const bool high_bit_depth = is_high_bit_depth_pix_fmt(hints.pix_fmt);
  const bool high_resolution = (hints.width > 1920 || hints.height > 1080);
  const bool high_fps = (hints.fps >= 48.0);

  if (heavy_codec || high_bit_depth || high_resolution || high_fps) {
    return std::string(kNativeHwaccel);
  }
  return "";
}

std::string resolve_effective_skip_frame(std::string_view requested_skip_frame,
                                         const StreamDecodeHints& hints,
                                         double sample_fps) {
  std::string trimmed = to_lower(trim(requested_skip_frame));
  if (trimmed.empty() || trimmed == "auto") {
    if (std::isfinite(hints.fps) && std::isfinite(sample_fps) &&
        sample_fps > 0.0 && hints.fps >= kAutoSkipNorefMinSourceFps &&
        hints.fps >= sample_fps * kAutoSkipNorefMinFpsRatio) {
      return "noref";
    }
    return "";
  }
  if (trimmed == "none" || trimmed == "off" || trimmed == "false" ||
      trimmed == "0" || trimmed == "default") {
    return "";
  }
  if (trimmed == "noref" || trimmed == "nonref") {
    return "noref";
  }
  throw std::invalid_argument("不支持的 skip_frame 模式: " +
                              std::string(requested_skip_frame));
}

ScaledOutputVfPlan plan_scaled_output_vf(
    double fps,
    const std::optional<SourceBox>& crop,
    std::int32_t source_width,
    std::int32_t source_height,
    const FfmpegExtractorOptions& options,
    std::string_view hw_mode,
    std::string_view pix_fmt) {
  std::int32_t raw_w = source_width;
  std::int32_t raw_h = source_height;
  if (crop.has_value()) {
    validate_output_crop(*crop, source_width, source_height);
    raw_w = crop->width;
    raw_h = crop->height;
  }

  std::string vf =
      options.enable_two_stage_crop
          ? build_optimized_output_vf(fps, crop, source_width, source_height, options.crop_margin)
          : build_output_vf(fps, crop);

  const bool use_nv12_bridge =
      !hw_mode.empty() && is_high_bit_depth_pix_fmt(pix_fmt);
  if (use_nv12_bridge) {
    const std::size_t pos = vf.find("format=rgb24");
    if (pos != std::string::npos) {
      vf.insert(pos, "format=nv12,");
    } else {
      vf += ",format=nv12,format=rgb24";
    }
  }

  std::int32_t out_w = raw_w;
  std::int32_t out_h = raw_h;
  bool scaled = false;
  if (options.max_output_width > 0 && raw_w > options.max_output_width && raw_h > 0) {
    const std::int32_t clamped_max =
        std::max<std::int32_t>(2, (options.max_output_width / 2) * 2);
    if (raw_w > clamped_max) {
      out_w = clamped_max;
      const double scaled_h =
          static_cast<double>(raw_h) * static_cast<double>(out_w) / static_cast<double>(raw_w);
      out_h = std::max<std::int32_t>(
          2, static_cast<std::int32_t>(std::llround(scaled_h / 2.0) * 2));
      scaled = true;
      vf += ",scale=" + std::to_string(out_w) + ":" + std::to_string(out_h) +
            ":flags=fast_bilinear";
    }
  }

  return ScaledOutputVfPlan{
      .vf = std::move(vf),
      .out_width = out_w,
      .out_height = out_h,
      .scaled = scaled,
      .used_nv12_bridge = use_nv12_bridge,
  };
}

}  // namespace sublift::adapters::ffmpeg
