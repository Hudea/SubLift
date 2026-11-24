#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>

#include <sublift/core/box.hpp>
#include <sublift/core/color.hpp>
#include <sublift/core/frame.hpp>
#include <sublift/core/image_buffer.hpp>
#include <sublift/core/image_view.hpp>
#include <sublift/core/subtitle.hpp>
#include <sublift/core/time.hpp>

namespace sublift {

inline constexpr std::string_view SCRIPT_CJK = "cjk";
inline constexpr std::string_view SCRIPT_LATIN = "latin";
inline constexpr std::string_view SCRIPT_AUTO = "auto";

[[nodiscard]] constexpr bool is_valid_script(std::string_view script) noexcept {
  return script == SCRIPT_CJK || script == SCRIPT_LATIN || script == SCRIPT_AUTO;
}

struct OcrLine {
  std::string text;
  double confidence{0.0};
  OcrCropBox box{};

  friend bool operator==(const OcrLine&, const OcrLine&) = default;
};

struct SubtitleProfile {
  std::string script{std::string{SCRIPT_AUTO}};
  std::int32_t center_x{0};
  std::int32_t center_y{0};
  std::int32_t height{0};
  std::int32_t y_min{0};
  std::int32_t y_max{0};

  friend bool operator==(const SubtitleProfile&, const SubtitleProfile&) = default;

  /// Full-band centered profile
  static SubtitleProfile from_crop(std::int32_t width, std::int32_t height,
                                   std::string_view script = SCRIPT_AUTO) {
    const std::int32_t w = std::max(std::int32_t{0}, width);
    const std::int32_t h = std::max(std::int32_t{0}, height);
    return SubtitleProfile{
        .script = std::string{script},
        .center_x = w / 2,
        .center_y = h / 2,
        .height = h,
        .y_min = 0,
        .y_max = h,
    };
  }

  /// Map video-pixel selection into crop profile
  static SubtitleProfile from_selection_in_video(
      SourceBox region, SourceBox selection, std::string_view script = SCRIPT_AUTO) {
    // Use int64 for intermediate geometry so extreme boxes do not wrap int32.
    const std::int64_t rel_x =
        static_cast<std::int64_t>(selection.x) - static_cast<std::int64_t>(region.x);
    const std::int64_t rel_y =
        static_cast<std::int64_t>(selection.y) - static_cast<std::int64_t>(region.y);
    const std::int32_t max_w = std::max(std::int32_t{0}, region.width);
    const std::int32_t max_h = std::max(std::int32_t{0}, region.height);

    const std::int32_t x0 = clamp_i32(rel_x, 0, max_w);
    const std::int32_t y0 = clamp_i32(rel_y, 0, max_h);
    const std::int64_t x1_raw = rel_x + static_cast<std::int64_t>(selection.width);
    const std::int64_t y1_raw = rel_y + static_cast<std::int64_t>(selection.height);
    const std::int32_t x1 = std::max(x0, clamp_i32(x1_raw, 0, max_w));
    const std::int32_t y1 = std::max(y0, clamp_i32(y1_raw, 0, max_h));
    const std::int32_t band_w = std::max(std::int32_t{0}, x1 - x0);
    const std::int32_t band_h = std::max(std::int32_t{0}, y1 - y0);

    return SubtitleProfile{
        .script = std::string{script},
        .center_x = x0 + band_w / 2,
        .center_y = y0 + band_h / 2,
        .height = band_h > 0 ? band_h : max_h,
        .y_min = y0,
        .y_max = y1 > y0 ? y1 : max_h,
    };
  }

  /// Construct profile from measured character height and vertical band
  static SubtitleProfile from_character_band(
      std::int32_t width, std::int32_t y_min, std::int32_t y_max,
      std::int32_t h_char, std::string_view script = SCRIPT_AUTO) {
    const std::int32_t w = std::max(std::int32_t{0}, width);
    const std::int32_t y0 = std::max(std::int32_t{0}, y_min);
    const std::int32_t y1 = std::max(y0, y_max);
    return SubtitleProfile{
        .script = std::string{script},
        .center_x = w / 2,
        .center_y = y0 + (y1 - y0) / 2,
        .height = h_char > 0 ? h_char : std::max(std::int32_t{1}, y1 - y0),
        .y_min = y0,
        .y_max = y1,
    };
  }

 private:
  [[nodiscard]] static constexpr std::int32_t clamp_i32(
      std::int64_t value, std::int32_t lo, std::int32_t hi) noexcept {
    if (value < lo) {
      return lo;
    }
    if (value > hi) {
      return hi;
    }
    return static_cast<std::int32_t>(value);
  }
};

} // namespace sublift
