#include "sublift/models.hpp"

#include <algorithm>
#include <cstdint>
#include <numeric>

namespace sublift {
namespace {

[[nodiscard]] std::int32_t clamp_i32(std::int64_t value, std::int32_t lo, std::int32_t hi) {
  if (value < lo) {
    return lo;
  }
  if (value > hi) {
    return hi;
  }
  return static_cast<std::int32_t>(value);
}

}  // namespace

SubtitleProfile SubtitleProfile::from_crop(std::int32_t width, std::int32_t height,
                                           std::string_view script) {
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

SubtitleProfile SubtitleProfile::from_selection_in_video(SourceBox region,
                                                         SourceBox selection,
                                                         std::string_view script) {
  // Use int64 for intermediate geometry so extreme boxes match Python arbitrary ints
  // rather than wrapping int32.
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

OcrResult OcrResult::from_lines(std::span<const OcrLine> lines) {
  if (lines.empty()) {
    return OcrResult{.text = "", .confidence = 0.0, .lines = {}};
  }
  std::vector<OcrLine> owned(lines.begin(), lines.end());
  std::string text;
  for (std::size_t i = 0; i < owned.size(); ++i) {
    if (i > 0) {
      text.push_back('\n');
    }
    text += owned[i].text;
  }
  const double conf =
      std::accumulate(owned.begin(), owned.end(), 0.0,
                      [](double acc, const OcrLine& line) {
                        return acc + line.confidence;
                      }) /
      static_cast<double>(owned.size());
  return OcrResult{.text = std::move(text), .confidence = conf, .lines = std::move(owned)};
}

}  // namespace sublift
