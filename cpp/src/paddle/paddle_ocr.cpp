#include "sublift/paddle.hpp"

#include <stdexcept>

namespace sublift {

#if defined(SUBLIFT_HAS_PADDLE) && SUBLIFT_HAS_PADDLE

bool is_paddle_available() noexcept {
  return true;
}

struct PaddleOcrEngine::Impl {
  PaddleOcrOptions options;

  explicit Impl(PaddleOcrOptions opts) : options(std::move(opts)) {
    // ONNX Runtime session & models initialization will be implemented in feat-06703
  }
};

PaddleOcrEngine::PaddleOcrEngine(PaddleOcrOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

PaddleOcrEngine::~PaddleOcrEngine() = default;

PaddleOcrEngine::PaddleOcrEngine(PaddleOcrEngine&&) noexcept = default;
PaddleOcrEngine& PaddleOcrEngine::operator=(PaddleOcrEngine&&) noexcept = default;

OcrResult PaddleOcrEngine::recognize(const ImageView& /*image*/) {
  // Recognize main path will be implemented in feat-06703
  return OcrResult::from_lines({});
}

#else

bool is_paddle_available() noexcept {
  return false;
}

struct PaddleOcrEngine::Impl {};

PaddleOcrEngine::PaddleOcrEngine(PaddleOcrOptions /*options*/) {
  throw std::runtime_error("PaddleOCR 不可用：C++ 构建未启用 SUBLIFT_ENABLE_PADDLE");
}

PaddleOcrEngine::~PaddleOcrEngine() = default;

PaddleOcrEngine::PaddleOcrEngine(PaddleOcrEngine&&) noexcept = default;
PaddleOcrEngine& PaddleOcrEngine::operator=(PaddleOcrEngine&&) noexcept = default;

OcrResult PaddleOcrEngine::recognize(const ImageView& /*image*/) {
  throw std::runtime_error("PaddleOCR 不可用：C++ 构建未启用 SUBLIFT_ENABLE_PADDLE");
}

#endif

}  // namespace sublift
