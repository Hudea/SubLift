#pragma once

#include <cstdint>
#include <vector>
#include <sublift/core/image_buffer.hpp>

namespace sublift::timing {

struct Segment {
  std::int64_t start_ms{0};
  std::int64_t end_ms{0};
  std::int64_t anchor_timestamp_ms{0};
  std::vector<std::int64_t> fallback_timestamps_ms{};
  ImageBuffer anchor_image{};
  std::vector<ImageBuffer> fallback_images{};

  [[nodiscard]] constexpr std::int64_t duration_ms() const noexcept {
    return end_ms - start_ms;
  }

  [[nodiscard]] Segment clone() const;
};

} // namespace sublift::timing
