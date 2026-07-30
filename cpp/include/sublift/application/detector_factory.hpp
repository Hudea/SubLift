#pragma once

#include <cstdint>
#include <memory>

#include "sublift/detector.hpp"
#include "sublift/models.hpp"

namespace sublift::application {

/// Builds IDetector instances without application including concrete adapter headers.
class IDetectorFactory {
 public:
  virtual ~IDetectorFactory() = default;

  [[nodiscard]] virtual std::unique_ptr<sublift::IDetector> make_fixed(
      sublift::FrameLocalBox box) const = 0;

  [[nodiscard]] virtual std::unique_ptr<sublift::IDetector> make_bottom_crop(
      double bottom_ratio) const = 0;

  [[nodiscard]] virtual std::unique_ptr<sublift::IDetector> make_roi_passthrough(
      std::int32_t width, std::int32_t height) const = 0;
};

}  // namespace sublift::application
