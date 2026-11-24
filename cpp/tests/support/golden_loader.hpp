#pragma once

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <sublift/core/color.hpp>
#include <sublift/core/image_buffer.hpp>
#include <sublift/core/image_view.hpp>
#include <sublift/timing/changepoint.hpp>
#include <sublift/timing/timing_config.hpp>

namespace sublift::test {

using json = nlohmann::json;

/// Load a JSON golden file from disk into a nlohmann::json structure.
inline json load_golden_json(const std::filesystem::path& file_path) {
  std::ifstream f(file_path);
  if (!f.is_open()) {
    throw std::runtime_error("Failed to open golden file: " + file_path.string());
  }
  json j;
  f >> j;
  return j;
}

/// Load raw RGB planar/interleaved bytes from a fixture file.
inline ImageBuffer load_crop_rgb(const std::filesystem::path& file_path, int width, int height) {
  std::ifstream in(file_path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("Failed to open crop file: " + file_path.string());
  }
  const std::size_t nbytes = static_cast<std::size_t>(width) * height * 3;
  std::vector<std::uint8_t> buf(nbytes);
  in.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(nbytes));
  ImageBuffer image = ImageBuffer::allocate_uninitialized(width, height, PixelFormat::RGB24);
  for (int y = 0; y < height; ++y) {
    std::memcpy(image.row(y), buf.data() + y * width * 3, width * 3);
  }
  return image;
}

/// Convert JSON value to uint64 supporting both number and decimal string formats.
inline std::uint64_t get_uint64(const json& v) {
  if (v.is_string()) {
    return std::strtoull(v.get<std::string>().c_str(), nullptr, 10);
  }
  return v.get<std::uint64_t>();
}

/// Parse TimingConfig from JSON with automatic backward compatibility for frame-based legacy fields.
inline timing::TimingConfig parse_timing_config(const json& j) {
  timing::TimingConfig cfg;
  if (j.contains("presence_threshold")) {
    cfg.presence_threshold = j["presence_threshold"].get<double>();
  }
  if (j.contains("change_threshold")) {
    cfg.change_threshold = j["change_threshold"].get<std::int32_t>();
  }
  if (j.contains("enable_ssim_verify")) {
    cfg.enable_ssim_verify = j["enable_ssim_verify"].get<bool>();
  }
  if (j.contains("ssim_threshold")) {
    cfg.ssim_threshold = j["ssim_threshold"].get<double>();
  }
  if (j.contains("ssim_window_size")) {
    cfg.ssim_window_size = j["ssim_window_size"].get<int>();
  }
  if (j.contains("enable_ssim_patrol")) {
    cfg.enable_ssim_patrol = j["enable_ssim_patrol"].get<bool>();
  }
  if (j.contains("ssim_patrol_threshold")) {
    cfg.ssim_patrol_threshold = j["ssim_patrol_threshold"].get<double>();
  }
  if (j.contains("ssim_patrol_use_mask")) {
    cfg.ssim_patrol_use_mask = j["ssim_patrol_use_mask"].get<bool>();
  }
  if (j.contains("aspect_ratio_threshold")) {
    cfg.aspect_ratio_threshold = j["aspect_ratio_threshold"].get<double>();
  }

  // Backward compatibility mapping for frame-based parameters
  if (j.contains("hysteresis_ms")) {
    cfg.hysteresis_ms = j["hysteresis_ms"].get<std::int64_t>();
  } else if (j.contains("hysteresis_frames")) {
    const int hf = j["hysteresis_frames"].get<int>();
    cfg.hysteresis_ms = (hf <= 1) ? 0 : static_cast<std::int64_t>(hf - 1) * 200;
  }

  if (j.contains("confirm_ms")) {
    cfg.confirm_ms = j["confirm_ms"].get<std::int64_t>();
  }

  if (j.contains("patrol_interval_ms")) {
    cfg.patrol_interval_ms = j["patrol_interval_ms"].get<std::int64_t>();
  } else if (j.contains("ssim_patrol_interval")) {
    cfg.patrol_interval_ms = static_cast<std::int64_t>(j["ssim_patrol_interval"].get<int>()) * 200;
  }

  if (j.contains("ocr_anchor_delay_ms")) {
    cfg.ocr_anchor_delay_ms = j["ocr_anchor_delay_ms"].get<std::int64_t>();
  } else if (j.contains("ocr_anchor_delay_frames")) {
    cfg.ocr_anchor_delay_ms = static_cast<std::int64_t>(j["ocr_anchor_delay_frames"].get<int>()) * 200;
  }

  if (j.contains("ocr_consensus_frames")) {
    cfg.ocr_consensus_frames = j["ocr_consensus_frames"].get<std::int32_t>();
  }
  if (j.contains("pure_timing")) {
    cfg.pure_timing = j["pure_timing"].get<bool>();
  }

  return cfg;
}

inline std::string event_type_to_string(timing::EventType t) {
  switch (t) {
    case timing::EventType::In: return "IN";
    case timing::EventType::Out: return "OUT";
    case timing::EventType::Change: return "CHANGE";
  }
  return "UNKNOWN";
}

inline timing::EventType string_to_event_type(std::string_view s) {
  if (s == "IN") return timing::EventType::In;
  if (s == "OUT") return timing::EventType::Out;
  if (s == "CHANGE") return timing::EventType::Change;
  throw std::runtime_error("Unknown event type: " + std::string(s));
}

} // namespace sublift::test
