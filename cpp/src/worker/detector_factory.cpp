#include "detector_factory.hpp"

#include "sublift/adapters/bottom_crop_detector.hpp"
#include "sublift/adapters/fixed_detector.hpp"
#include "sublift/adapters/roi_passthrough_detector.hpp"

namespace sublift::worker {

std::unique_ptr<sublift::IDetector> DefaultDetectorFactory::make_fixed(
    sublift::FrameLocalBox box) const {
  return std::make_unique<sublift::FixedRegionDetector>(box);
}

std::unique_ptr<sublift::IDetector> DefaultDetectorFactory::make_bottom_crop(
    double bottom_ratio) const {
  return std::make_unique<sublift::BottomCropDetector>(bottom_ratio);
}

std::unique_ptr<sublift::IDetector> DefaultDetectorFactory::make_roi_passthrough(
    std::int32_t width, std::int32_t height) const {
  return std::make_unique<sublift::RoiPassthroughDetector>(width, height);
}

}  // namespace sublift::worker
