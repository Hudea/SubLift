#pragma once

#include <optional>

#include "sublift/detector.hpp"
#include "sublift/models.hpp"

namespace sublift {

/// Test-only detector returning a fixed region. Mirrors
/// `src/sublift/detector/fixed_region.py` (`FixedRegionDetector`).
///
/// Ignores the frame's actual size and always returns the region supplied at
/// construction. Bounds checking is the downstream's responsibility (same as
/// Python). Detection here always resolves, so `detect` never returns nullopt.
class FixedRegionDetector final : public IDetector {
 public:
  explicit FixedRegionDetector(FrameLocalBox box) : region_{Region{box}} {}

  explicit FixedRegionDetector(Region region) : region_{region} {}

  [[nodiscard]] std::optional<Region> detect(const Frame& /*frame*/) override {
    return region_;
  }

 private:
  Region region_{};
};

}  // namespace sublift
