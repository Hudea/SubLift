#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <string_view>

#include <sublift/core/box.hpp>
#include <sublift/core/models.hpp>

namespace sublift::adapters::apple_vision {

/// Banker's Rounding (round half to even) matching Python 3 round().
[[nodiscard]] inline std::int32_t bankers_round(double v) noexcept {
  if (!std::isfinite(v)) {
    return 0;
  }
  constexpr double kMinI32 = -2147483648.0;
  constexpr double kMaxI32 = 2147483647.0;
  if (v <= kMinI32) {
    return std::numeric_limits<std::int32_t>::min();
  }
  if (v >= kMaxI32) {
    return std::numeric_limits<std::int32_t>::max();
  }
  const double r = std::round(v);
  const double diff = r - v;
  if (diff == 0.5 || diff == -0.5) {
    const auto int_r = static_cast<std::int64_t>(r);
    if (int_r % 2 != 0) {
      const auto adjusted = int_r - (diff > 0 ? 1 : -1);
      if (adjusted < static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::min())) {
        return std::numeric_limits<std::int32_t>::min();
      }
      if (adjusted > static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max())) {
        return std::numeric_limits<std::int32_t>::max();
      }
      return static_cast<std::int32_t>(adjusted);
    }
  }
  return static_cast<std::int32_t>(r);
}

/// Clamp pixel box into [0, image_width] x [0, image_height].
/// If image_width <= 0 or image_height <= 0, returns OcrCropBox{0, 0, 0, 0}.
[[nodiscard]] OcrCropBox clamp_ocr_box(
    std::int32_t x, std::int32_t y,
    std::int32_t w, std::int32_t h,
    std::int32_t image_width, std::int32_t image_height) noexcept;

/// Convert Vision normalized box (origin bottom-left, [0..1]) into pixel OcrCropBox
/// (origin top-left, [0..W], [0..H]) and clamp into image boundary using Banker's rounding.
[[nodiscard]] OcrCropBox vision_normalized_box_to_pixel(
    double nx, double ny, double nw, double nh,
    std::int32_t image_width, std::int32_t image_height) noexcept;

/// Stable sort OCR recognition lines (primary key: y ascending, secondary key: x ascending).
void sort_ocr_lines(std::span<OcrLine> lines);

/// True if `text` is empty after Unicode whitespace stripping (ASCII + Unicode Zs,
/// including U+00A0 NBSP and U+3000 ideographic space).
[[nodiscard]] bool is_ocr_text_blank(std::string_view text) noexcept;

}  // namespace sublift::adapters::apple_vision
