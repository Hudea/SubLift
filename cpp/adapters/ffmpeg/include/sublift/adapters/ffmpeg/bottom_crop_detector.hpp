#pragma once

#include <cmath>
#include <cstdint>
#include <optional>

#include <sublift/core/box.hpp>
#include <sublift/core/frame.hpp>
#include <sublift/ports/detector.hpp>

namespace sublift::adapters::ffmpeg {

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

 private:
  double bottom_ratio_{0.3};
};

}  // namespace sublift::adapters::ffmpeg
