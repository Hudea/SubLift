#pragma once

#include "sublift/application/detector_factory.hpp"

namespace sublift::worker {

class DefaultDetectorFactory final : public sublift::application::IDetectorFactory {
 public:
  [[nodiscard]] std::unique_ptr<sublift::IDetector> make_fixed(
      sublift::FrameLocalBox box) const override;

  [[nodiscard]] std::unique_ptr<sublift::IDetector> make_bottom_crop(
      double bottom_ratio) const override;

  [[nodiscard]] std::unique_ptr<sublift::IDetector> make_roi_passthrough(
      std::int32_t width, std::int32_t height) const override;
};

}  // namespace sublift::worker
