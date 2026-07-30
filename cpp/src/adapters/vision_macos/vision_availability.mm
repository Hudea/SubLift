#import <Foundation/Foundation.h>
#import <Vision/Vision.h>

#include "sublift/adapters/vision.hpp"

namespace sublift {

bool is_vision_available() noexcept {
  return [VNRecognizeTextRequest class] != nil;
}

}  // namespace sublift
