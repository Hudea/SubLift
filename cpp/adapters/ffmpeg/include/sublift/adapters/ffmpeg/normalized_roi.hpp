#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>

#include <sublift/core/box.hpp>

namespace sublift::adapters::ffmpeg {

struct NormalizedRegionBox {
  double x{0.0};
  double y{0.0};
  double width{0.0};
  double height{0.0};

  [[nodiscard]] Box2i to_pixel_box(std::int32_t frame_width,
                                   std::int32_t frame_height) const {
    if (frame_width <= 0 || frame_height <= 0) {
      throw std::invalid_argument("region box requires a non-empty frame");
    }
    const auto clamp01 = [](double value) {
      return std::clamp(value, 0.0, 1.0);
    };
    const auto px_x = std::clamp(
        static_cast<std::int32_t>(std::round(clamp01(x) * frame_width)), 0,
        frame_width - 1);
    const auto px_y = std::clamp(
        static_cast<std::int32_t>(std::round(clamp01(y) * frame_height)), 0,
        frame_height - 1);
    const auto px_w = std::clamp(
        static_cast<std::int32_t>(std::round(clamp01(width) * frame_width)), 1,
        frame_width - px_x);
    const auto px_h = std::clamp(
        static_cast<std::int32_t>(std::round(clamp01(height) * frame_height)), 1,
        frame_height - px_y);
    return Box2i{px_x, px_y, px_w, px_h};
  }

  [[nodiscard]] SourceBox to_source_box(std::int32_t frame_width,
                                        std::int32_t frame_height) const {
    auto b = to_pixel_box(frame_width, frame_height);
    return SourceBox{b.x, b.y, b.width, b.height};
  }
};

}  // namespace sublift::adapters::ffmpeg
