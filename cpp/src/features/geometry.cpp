#include <sublift/features/geometry.hpp>

namespace sublift::features {

bool is_valid_text_region(const FrameLocalBox& box, int frame_width, int frame_height,
                          const CorridorConfig& config) {
  if (box.empty() || frame_width <= 0 || frame_height <= 0) {
    return false;
  }
  if (box.x < 0 || box.y < 0 || box.right() > frame_width || box.bottom() > frame_height) {
    return false;
  }
  double aspect = static_cast<double>(box.width) / static_cast<double>(box.height);
  if (aspect < config.min_aspect_ratio || aspect > config.max_aspect_ratio) {
    return false;
  }
  const double frame_area = static_cast<double>(frame_width) * static_cast<double>(frame_height);
  const double box_area = static_cast<double>(box.width) * static_cast<double>(box.height);
  if ((box_area / frame_area) < config.min_area_fraction) {
    return false;
  }
  return true;
}

} // namespace sublift::features
