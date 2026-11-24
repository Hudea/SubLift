#pragma once

#include <sublift/core/box.hpp>

namespace sublift::features {

struct CorridorConfig {
  double min_aspect_ratio{1.5};
  double max_aspect_ratio{25.0};
  double min_area_fraction{0.001};
};

[[nodiscard]] bool is_valid_text_region(const FrameLocalBox& box, int frame_width, int frame_height,
                                        const CorridorConfig& config = {});

} // namespace sublift::features
