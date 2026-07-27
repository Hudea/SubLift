#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "sublift/image.hpp"

namespace sublift {

/// Shared geometry shape (not a coordinate space). Prefer strong boxes below
/// for API boundaries so spaces cannot be mixed silently.
struct Box2i {
  std::int32_t x{0};
  std::int32_t y{0};
  std::int32_t width{0};
  std::int32_t height{0};

  [[nodiscard]] constexpr bool empty() const noexcept {
    return width <= 0 || height <= 0;
  }

  friend constexpr bool operator==(const Box2i&, const Box2i&) = default;
};

// Strong boxes: same fields as Box2i but distinct types (no inheritance so
// C++20 designated initializers work and accidental cross-space assignment fails).

/// source-frame (full frame / GUI region / ffmpeg crop params)
struct SourceBox {
  std::int32_t x{0};
  std::int32_t y{0};
  std::int32_t width{0};
  std::int32_t height{0};
  friend constexpr bool operator==(const SourceBox&, const SourceBox&) = default;
};
/// extractor output image (post-ROI frame-local)
struct FrameLocalBox {
  std::int32_t x{0};
  std::int32_t y{0};
  std::int32_t width{0};
  std::int32_t height{0};
  friend constexpr bool operator==(const FrameLocalBox&,
                                   const FrameLocalBox&) = default;
};
/// image passed to OCR; OcrLine.box lives here
struct OcrCropBox {
  std::int32_t x{0};
  std::int32_t y{0};
  std::int32_t width{0};
  std::int32_t height{0};
  friend constexpr bool operator==(const OcrCropBox&, const OcrCropBox&) = default;
};

struct Region {
  FrameLocalBox box{};
};

struct Frame {
  std::int64_t timestamp_ms{0};
  ImageBuffer image{};
};

struct OcrLine {
  std::string text;
  double confidence{0.0};
  OcrCropBox box{};
};

inline constexpr std::string_view SCRIPT_CJK = "cjk";
inline constexpr std::string_view SCRIPT_LATIN = "latin";
inline constexpr std::string_view SCRIPT_AUTO = "auto";

[[nodiscard]] constexpr bool is_valid_script(std::string_view script) noexcept {
  return script == SCRIPT_CJK || script == SCRIPT_LATIN || script == SCRIPT_AUTO;
}

struct SubtitleProfile {
  std::string script{std::string{SCRIPT_AUTO}};
  std::int32_t center_x{0};
  std::int32_t center_y{0};
  std::int32_t height{0};
  std::int32_t y_min{0};
  std::int32_t y_max{0};

  friend bool operator==(const SubtitleProfile&, const SubtitleProfile&) = default;

  /// CLI/benchmark: full-band centered profile (matches Python from_crop).
  static SubtitleProfile from_crop(std::int32_t width, std::int32_t height,
                                   std::string_view script = SCRIPT_AUTO);

  /// Map video-pixel selection into OCR-crop profile (matches Python).
  static SubtitleProfile from_selection_in_video(
      SourceBox region, SourceBox selection, std::string_view script = SCRIPT_AUTO);
};

struct OcrResult {
  std::string text;
  double confidence{0.0};
  std::vector<OcrLine> lines;

  static OcrResult from_lines(std::span<const OcrLine> lines);
};

struct SubtitleEntry {
  std::int64_t start_ms{0};
  std::int64_t end_ms{0};
  std::string text;
  double confidence{1.0};
};

}  // namespace sublift
