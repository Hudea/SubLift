#pragma once

#include <optional>

#include <sublift/core/box.hpp>
#include <sublift/core/frame.hpp>
#include <sublift/ports/detector.hpp>

namespace sublift::adapters::ffmpeg {

/// First-class core detector returning a fixed region.
/// Used by plan_frame_io for mode=full and display-transform fallback (FullRgb + FixedRegion).
class FixedRegionDetector final : public IDetector {
 public:
  explicit FixedRegionDetector(FrameLocalBox box) : region_{Region{box}} {}
  explicit FixedRegionDetector(SourceBox box)
      : region_{Region{FrameLocalBox{box.x, box.y, box.width, box.height}}} {}
  explicit FixedRegionDetector(Region region) : region_{region} {}

  [[nodiscard]] const Region& region() const noexcept { return region_; }

  [[nodiscard]] std::optional<Region> detect(const Frame& /*frame*/) override {
    return region_;
  }

 private:
  Region region_{};
};

}  // namespace sublift::adapters::ffmpeg
