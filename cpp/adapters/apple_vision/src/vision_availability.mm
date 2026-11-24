#import <Foundation/Foundation.h>
#import <Vision/Vision.h>

#include <sublift/adapters/apple_vision/apple_vision_ocr.hpp>

namespace sublift::adapters::apple_vision {

bool is_vision_available() noexcept {
  @autoreleasepool {
    return [VNRecognizeTextRequest class] != nil;
  }
}

AppleVisionCapabilities AppleVisionOcrEngine::probe_capabilities() noexcept {
  const bool avail = is_vision_available();
  return AppleVisionCapabilities{
      .available = avail,
      .detail = avail ? "ok (Apple Vision)" : "VNRecognizeTextRequest unavailable",
  };
}

}  // namespace sublift::adapters::apple_vision
