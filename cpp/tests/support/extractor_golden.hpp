#pragma once

#include <array>
#include <cstdint>
#include <fstream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <sublift/core/box.hpp>
#include <sublift/ports/frame_source.hpp>

namespace sublift::test {

using nlohmann::json;

struct ExtractorValidateCropGolden {
  std::string name;
  std::optional<SourceBox> crop;
  std::int32_t source_width{0};
  std::int32_t source_height{0};
  bool expected_ok{true};
  std::optional<std::string> expected_error_contains;
};

struct ExtractorBuildVfGolden {
  std::string name;
  double fps{0.0};
  std::optional<SourceBox> crop;
  std::string expected_vf;
};

struct ExtractorAssessDisplayGolden {
  std::string name;
  json stream_json;
  bool expected_ok{true};
  std::optional<std::string> expected_note;
};

struct ExtractorPlanFrameIoGolden {
  std::string name;
  std::optional<SourceBox> region_box;
  std::string mode;
  std::optional<SourceFrameInfo> source;
  std::string on_unvalidated_transform{"fallback_full"};
  bool expected_ok{true};
  std::optional<std::string> expected_error_contains;
  std::optional<SourceBox> expected_output_crop;
  std::string expected_output_mode{"full_rgb"};
  std::string expected_detector_type{"bottomcrop"};
  std::optional<std::string> expected_fallback_reason;
};

struct ExtractorSampledPixelGolden {
  std::int32_t x{0};
  std::int32_t y{0};
  std::array<int, 3> rgb{0, 0, 0};
};

struct ExtractorFrameGolden {
  std::int32_t frame_index{0};
  std::int64_t timestamp_ms{0};
  std::vector<ExtractorSampledPixelGolden> sampled_pixels;
};

struct ExtractorExtractScenarioGolden {
  std::string name;
  double fps{1.0};
  std::optional<SourceBox> crop;
  bool cancel_before_extract{false};
  bool expected_ok{true};
  std::optional<std::string> expected_error_contains;
  int expected_frame_count{0};
  std::vector<std::int64_t> expected_timestamps_ms;
  int expected_width{0};
  int expected_height{0};
  std::vector<ExtractorFrameGolden> frames;
};

struct ExtractorGolden {
  struct Oracle {
    std::string oracle_commit;
    std::optional<std::string> oracle_branch;
  } oracle;
  std::vector<ExtractorValidateCropGolden> pure_validate_crop;
  std::vector<ExtractorBuildVfGolden> pure_build_vf;
  std::vector<ExtractorAssessDisplayGolden> pure_assess_display;
  std::vector<ExtractorPlanFrameIoGolden> pure_plan_frame_io;
  std::vector<ExtractorExtractScenarioGolden> extract_scenarios;
};

namespace detail {

inline std::int32_t require_i32(const json& obj, const std::string& key) {
  if (!obj.contains(key)) throw std::runtime_error("missing int field: " + key);
  const auto& v = obj.at(key);
  if (!v.is_number_integer() && !v.is_number_unsigned()) {
    throw std::runtime_error("field not integer: " + key);
  }
  const auto wide = v.get<std::int64_t>();
  if (wide < std::numeric_limits<std::int32_t>::min() ||
      wide > std::numeric_limits<std::int32_t>::max()) {
    throw std::runtime_error("int field out of int32 range: " + key);
  }
  return static_cast<std::int32_t>(wide);
}

inline std::int64_t require_i64(const json& obj, const std::string& key) {
  if (!obj.contains(key)) throw std::runtime_error("missing int64 field: " + key);
  const auto& v = obj.at(key);
  if (!v.is_number_integer() && !v.is_number_unsigned()) {
    throw std::runtime_error("field not integer: " + key);
  }
  return v.get<std::int64_t>();
}

inline double require_f64(const json& obj, const std::string& key) {
  if (!obj.contains(key)) throw std::runtime_error("missing float field: " + key);
  const auto& v = obj.at(key);
  if (!v.is_number()) throw std::runtime_error("field not number: " + key);
  return v.get<double>();
}

inline std::string require_string(const json& obj, const std::string& key) {
  if (!obj.contains(key)) throw std::runtime_error("missing string field: " + key);
  const auto& v = obj.at(key);
  if (!v.is_string()) throw std::runtime_error("field not string: " + key);
  return v.get<std::string>();
}

inline bool require_bool(const json& obj, const std::string& key) {
  if (!obj.contains(key)) throw std::runtime_error("missing bool field: " + key);
  const auto& v = obj.at(key);
  if (!v.is_boolean()) throw std::runtime_error("field not bool: " + key);
  return v.get<bool>();
}

inline std::optional<SourceBox> parse_box(const json& parent, const std::string& key) {
  if (!parent.contains(key) || parent.at(key).is_null()) return std::nullopt;
  const auto& v = parent.at(key);
  SourceBox b;
  b.x = require_i32(v, "x");
  b.y = require_i32(v, "y");
  b.width = require_i32(v, "width");
  b.height = require_i32(v, "height");
  return b;
}

}  // namespace detail

inline ExtractorGolden load_extractor_golden(const std::filesystem::path& path) {
  std::ifstream ifs(path, std::ios::binary);
  if (!ifs) {
    throw std::runtime_error("cannot open extractor golden JSON: " + path.string());
  }

  json root_json;
  try {
    ifs >> root_json;
  } catch (const std::exception& pe) {
    throw std::runtime_error("extractor golden JSON parse error: " + std::string(pe.what()));
  }

  if (!root_json.is_object()) {
    throw std::runtime_error("extractor golden root must be object");
  }
  if (!root_json.contains("golden_schema_version") ||
      root_json.at("golden_schema_version").get<int>() != 1) {
    throw std::runtime_error("extractor golden golden_schema_version must be 1");
  }
  if (!root_json.contains("kind") || root_json.at("kind").get<std::string>() != "extractor") {
    throw std::runtime_error("extractor golden kind must be extractor");
  }

  ExtractorGolden res;
  if (root_json.contains("oracle")) {
    const auto& o = root_json.at("oracle");
    res.oracle.oracle_commit = detail::require_string(o, "oracle_commit");
    if (o.contains("oracle_branch") && !o.at("oracle_branch").is_null()) {
      res.oracle.oracle_branch = o.at("oracle_branch").get<std::string>();
    }
  }

  if (root_json.contains("pure_validate_crop")) {
    for (const auto& item : root_json.at("pure_validate_crop")) {
      ExtractorValidateCropGolden v;
      v.name = detail::require_string(item, "name");
      v.crop = detail::parse_box(item, "crop");
      v.source_width = detail::require_i32(item, "source_width");
      v.source_height = detail::require_i32(item, "source_height");
      v.expected_ok = detail::require_bool(item, "expected_ok");
      if (item.contains("expected_error_contains") && !item.at("expected_error_contains").is_null()) {
        v.expected_error_contains = item.at("expected_error_contains").get<std::string>();
      }
      res.pure_validate_crop.push_back(std::move(v));
    }
  }

  if (root_json.contains("pure_build_vf")) {
    for (const auto& item : root_json.at("pure_build_vf")) {
      ExtractorBuildVfGolden v;
      v.name = detail::require_string(item, "name");
      v.fps = detail::require_f64(item, "fps");
      v.crop = detail::parse_box(item, "crop");
      v.expected_vf = detail::require_string(item, "expected_vf");
      res.pure_build_vf.push_back(std::move(v));
    }
  }

  if (root_json.contains("pure_assess_display")) {
    for (const auto& item : root_json.at("pure_assess_display")) {
      ExtractorAssessDisplayGolden v;
      v.name = detail::require_string(item, "name");
      v.stream_json = item.at("stream_json");
      v.expected_ok = detail::require_bool(item, "expected_ok");
      if (item.contains("expected_note") && !item.at("expected_note").is_null()) {
        v.expected_note = item.at("expected_note").get<std::string>();
      }
      res.pure_assess_display.push_back(std::move(v));
    }
  }

  if (root_json.contains("pure_plan_frame_io")) {
    for (const auto& item : root_json.at("pure_plan_frame_io")) {
      ExtractorPlanFrameIoGolden v;
      v.name = detail::require_string(item, "name");
      v.region_box = detail::parse_box(item, "region_box");
      v.mode = detail::require_string(item, "mode");
      if (item.contains("source") && !item.at("source").is_null()) {
        const auto& s = item.at("source");
        SourceFrameInfo info;
        info.width = detail::require_i32(s, "width");
        info.height = detail::require_i32(s, "height");
        info.display_transform_ok = detail::require_bool(s, "display_transform_ok");
        if (s.contains("transform_note") && !s.at("transform_note").is_null()) {
          info.transform_note = s.at("transform_note").get<std::string>();
        }
        v.source = info;
      }
      if (item.contains("on_unvalidated_transform") &&
          !item.at("on_unvalidated_transform").is_null()) {
        v.on_unvalidated_transform = item.at("on_unvalidated_transform").get<std::string>();
      }
      if (item.contains("expected_ok")) {
        v.expected_ok = detail::require_bool(item, "expected_ok");
      }
      if (item.contains("expected_error_contains") &&
          !item.at("expected_error_contains").is_null()) {
        v.expected_error_contains = item.at("expected_error_contains").get<std::string>();
      }
      if (v.expected_ok) {
        v.expected_output_crop = detail::parse_box(item, "expected_output_crop");
        v.expected_output_mode = detail::require_string(item, "expected_output_mode");
        v.expected_detector_type = detail::require_string(item, "expected_detector_type");
        if (item.contains("expected_fallback_reason") &&
            !item.at("expected_fallback_reason").is_null()) {
          v.expected_fallback_reason = item.at("expected_fallback_reason").get<std::string>();
        }
      }
      res.pure_plan_frame_io.push_back(std::move(v));
    }
  }

  if (root_json.contains("extract_scenarios")) {
    for (const auto& item : root_json.at("extract_scenarios")) {
      ExtractorExtractScenarioGolden v;
      v.name = detail::require_string(item, "name");
      v.fps = detail::require_f64(item, "fps");
      v.crop = detail::parse_box(item, "crop");
      if (item.contains("cancel_before_extract")) {
        v.cancel_before_extract = item.at("cancel_before_extract").get<bool>();
      }
      if (item.contains("expected_ok")) {
        v.expected_ok = detail::require_bool(item, "expected_ok");
      }
      if (item.contains("expected_error_contains") &&
          !item.at("expected_error_contains").is_null()) {
        v.expected_error_contains = item.at("expected_error_contains").get<std::string>();
      }
      if (item.contains("expected_frame_count") && !item.at("expected_frame_count").is_null()) {
        v.expected_frame_count = detail::require_i32(item, "expected_frame_count");
      }
      if (item.contains("expected_timestamps_ms") && !item.at("expected_timestamps_ms").is_null()) {
        for (const auto& ts : item.at("expected_timestamps_ms")) {
          v.expected_timestamps_ms.push_back(ts.get<std::int64_t>());
        }
      }
      if (item.contains("expected_width") && !item.at("expected_width").is_null()) {
        v.expected_width = detail::require_i32(item, "expected_width");
      }
      if (item.contains("expected_height") && !item.at("expected_height").is_null()) {
        v.expected_height = detail::require_i32(item, "expected_height");
      }

      if (item.contains("frames") && !item.at("frames").is_null()) {
        for (const auto& f_json : item.at("frames")) {
          ExtractorFrameGolden fg;
          fg.frame_index = detail::require_i32(f_json, "frame_index");
          fg.timestamp_ms = detail::require_i64(f_json, "timestamp_ms");
          if (f_json.contains("sampled_pixels")) {
            for (const auto& px_json : f_json.at("sampled_pixels")) {
              ExtractorSampledPixelGolden px;
              px.x = detail::require_i32(px_json, "x");
              px.y = detail::require_i32(px_json, "y");
              if (px_json.contains("rgb")) {
                const auto& rgb_arr = px_json.at("rgb");
                if (rgb_arr.is_array() && rgb_arr.size() >= 3) {
                  px.rgb = {rgb_arr[0].get<int>(), rgb_arr[1].get<int>(), rgb_arr[2].get<int>()};
                }
              }
              fg.sampled_pixels.push_back(std::move(px));
            }
          }
          v.frames.push_back(std::move(fg));
        }
      }
      res.extract_scenarios.push_back(std::move(v));
    }
  }


  return res;
}

}  // namespace sublift::test
