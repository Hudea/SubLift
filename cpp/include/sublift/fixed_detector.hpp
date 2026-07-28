#pragma once

#include <optional>

#include "sublift/detector.hpp"
#include "sublift/models.hpp"

namespace sublift {

/// First-class core detector returning a fixed region. Mirrors
/// `src/sublift/detector/fixed_region.py` (`FixedRegionDetector`).
///
/// Used by product `plan_frame_io` for mode=full and display-transform
/// fallback (FullRgb + FixedRegion). Not test-only — reserve that label for
/// MockOcrEngine.
///
/// Ignores the frame's actual size and always returns the region supplied at
/// construction. Bounds checking is the downstream's responsibility (same as
/// Python). Detection here always resolves, so `detect` never returns nullopt.
class FixedRegionDetector final : public IDetector {
 public:
  explicit FixedRegionDetector(FrameLocalBox box) : region_{Region{box}} {}

  explicit FixedRegionDetector(Region region) : region_{region} {}

  [[nodiscard]] const Region& region() const noexcept { return region_; }

  [[nodiscard]] std::optional<Region> detect(const Frame& /*frame*/) override {
    return region_;
  }

  [[nodiscard]] bool is_equal(const IDetector& other) const noexcept override {
    if (auto* p = dynamic_cast<const FixedRegionDetector*>(&other)) {
      return region_.box == p->region_.box;
    }
    return false;
  }

 private:
  Region region_{};
};

}  // namespace sublift
