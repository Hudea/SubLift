#include <sublift/adapters/apple_vision/apple_vision_ocr.hpp>

#include <stdexcept>

namespace sublift::adapters::apple_vision {

struct AppleVisionOcrEngine::Impl {
  std::vector<std::string> recognition_languages;
  AppleVisionRuntimeStats runtime_stats;
};

bool is_vision_available() noexcept {
  return false;
}

AppleVisionCapabilities AppleVisionOcrEngine::probe_capabilities() noexcept {
  return AppleVisionCapabilities{
      .available = false,
      .detail = "SUBLIFT_ENABLE_VISION=OFF",
  };
}

AppleVisionOcrEngine::AppleVisionOcrEngine(AppleVisionOptions /*options*/) {
  throw std::runtime_error(
      "Apple Vision OCR engine is disabled at build time (SUBLIFT_ENABLE_VISION=OFF)");
}

AppleVisionOcrEngine::AppleVisionOcrEngine(
    std::vector<std::string> /*recognition_languages*/) {
  throw std::runtime_error(
      "Apple Vision OCR engine is disabled at build time (SUBLIFT_ENABLE_VISION=OFF)");
}

AppleVisionOcrEngine::~AppleVisionOcrEngine() = default;
AppleVisionOcrEngine::AppleVisionOcrEngine(AppleVisionOcrEngine&&) noexcept = default;
AppleVisionOcrEngine& AppleVisionOcrEngine::operator=(AppleVisionOcrEngine&&) noexcept = default;

OcrResult AppleVisionOcrEngine::recognize(const ImageView& /*image*/) {
  throw std::runtime_error(
      "Apple Vision OCR engine is disabled at build time (SUBLIFT_ENABLE_VISION=OFF)");
}

std::string AppleVisionOcrEngine::engine_name() const {
  return "apple_vision";
}

const std::vector<std::string>& AppleVisionOcrEngine::recognition_languages() const noexcept {
  static const std::vector<std::string> empty_langs;
  return impl_ ? impl_->recognition_languages : empty_langs;
}

AppleVisionRuntimeStats AppleVisionOcrEngine::runtime_stats() const noexcept {
  return impl_ ? impl_->runtime_stats : AppleVisionRuntimeStats{};
}

}  // namespace sublift::adapters::apple_vision
