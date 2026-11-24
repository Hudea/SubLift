#pragma once

#include <sublift/core/image_buffer.hpp>
#include <sublift/core/image_view.hpp>

namespace sublift::features {

struct MaskConfig {
  double block_size_ratio{0.08};
  int adaptive_block_size{0};
  double adaptive_c{12.0};
  int morph_kernel_width{2};
  int morph_kernel_height{2};
  double aspect_ratio_threshold{6.0};
  bool use_adaptive_mask{true};
  bool use_sector_features{true};
};

[[nodiscard]] ImageBuffer compute_binary_mask(const ImageView& image, const MaskConfig& config = {});
[[nodiscard]] double compute_foreground_ratio(const ImageView& mask);

} // namespace sublift::features
