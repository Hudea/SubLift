#pragma once

#include <cmath>
#include <cstdint>
#include <optional>

#include "sublift/detector.hpp"
#include "sublift/models.hpp"

namespace sublift {

class BottomCropDetector final : public IDetector {
 public:
  explicit BottomCropDetector(double bottom_ratio = 0.3)
      : bottom_ratio_{bottom_ratio} {}

  [[nodiscard]] double bottom_ratio() const noexcept { return bottom_ratio_; }

  [[nodiscard]] std::optional<Region> detect(const Frame& frame) override {
    const std::int32_t width = frame.image.width();
    const std::int32_t height = frame.image.height();
    const std::int32_t crop_height = static_cast<std::int32_t>(height * bottom_ratio_);
    FrameLocalBox box{
        .x = 0,
        .y = height - crop_height,
        .width = width,
        .height = crop_height,
    };
    return Region{box};
  }

  [[nodiscard]] bool is_equal(const IDetector& other) const noexcept override {
    if (auto* p = dynamic_cast<const BottomCropDetector*>(&other)) {
      return bottom_ratio_ == p->bottom_ratio_;
    }
    return false;
  }

 private:
  double bottom_ratio_{0.3};
};

}  // namespace sublift
