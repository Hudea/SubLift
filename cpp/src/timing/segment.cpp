#include <sublift/timing/segment.hpp>

namespace sublift::timing {

Segment Segment::clone() const {
  Segment s;
  s.start_ms = start_ms;
  s.end_ms = end_ms;
  s.anchor_timestamp_ms = anchor_timestamp_ms;
  s.fallback_timestamps_ms = fallback_timestamps_ms;
  s.anchor_image = anchor_image.clone();
  s.fallback_images.reserve(fallback_images.size());
  for (const auto& img : fallback_images) {
    s.fallback_images.push_back(img.clone());
  }
  return s;
}

} // namespace sublift::timing
