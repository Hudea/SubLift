#include <stdexcept>
#include "sublift/adapters/vision.hpp"

namespace sublift {

struct VisionOcrEngine::Impl {
  std::vector<std::string> recognition_languages;
};

bool is_vision_available() noexcept {
  return false;
}

VisionOcrEngine::VisionOcrEngine(std::vector<std::string> /*recognition_languages*/) {
  throw std::runtime_error(
      "Apple Vision OCR engine is disabled at build time (SUBLIFT_ENABLE_VISION=OFF)");
}

VisionOcrEngine::~VisionOcrEngine() = default;
VisionOcrEngine::VisionOcrEngine(VisionOcrEngine&&) noexcept = default;
VisionOcrEngine& VisionOcrEngine::operator=(VisionOcrEngine&&) noexcept = default;

OcrResult VisionOcrEngine::recognize(const ImageView& /*image*/) {
  return OcrResult{};
}

const std::vector<std::string>& VisionOcrEngine::recognition_languages() const noexcept {
  static const std::vector<std::string> empty_langs;
  return impl_ ? impl_->recognition_languages : empty_langs;
}

}  // namespace sublift
