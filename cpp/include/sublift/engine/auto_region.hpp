#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>

#include <sublift/core/box.hpp>
#include <sublift/core/models.hpp>
#include <sublift/ports/frame_source.hpp>
#include <sublift/ports/ocr_engine.hpp>

namespace sublift {

struct AutoRegionOptions {
  std::string script{"auto"};
  double bottom_ratio{0.45};
  std::size_t max_ocr_candidates{6};
  std::size_t min_ocr_candidates{2};
  double min_ocr_confidence{0.28};
  bool fallback_to_default{true};
};

struct AutoRegionResult {
  bool detected{false};
  SourceBox region_box{};           // 推荐裁剪区域（包含安全边界）
  SubtitleProfile profile{};         // 局部的 SubtitleProfile（相对 region_box 坐标）
  std::int64_t sample_timestamp_ms{0};
  std::string preview_text{};
  double confidence{0.0};
  std::string error_message{};
};

[[nodiscard]] AutoRegionResult detect_auto_region(
    IFrameSource& source,
    IOcrEngine& ocr,
    const std::filesystem::path& video_path,
    const AutoRegionOptions& options = {});

} // namespace sublift
