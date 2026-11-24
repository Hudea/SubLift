#pragma once

#include <cstdint>
#include <sublift/core/image_buffer.hpp>
#include <sublift/core/image_view.hpp>
#include <sublift/features/mask.hpp>

namespace sublift::features {

struct AdaptiveMaskResult {
  ImageBuffer valid_mask;
  double foreground_ratio{0.0};
  std::uint64_t dhash{0};
};

[[nodiscard]] AdaptiveMaskResult compute_adaptive_presence_mask(
    const ImageView& image, const MaskConfig& config);

} // namespace sublift::features
