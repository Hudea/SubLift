#include "sublift/paddle.hpp"

#include <stdexcept>
#include <vector>

#include "ppocr_ctc.hpp"

namespace sublift {

#if defined(SUBLIFT_HAS_PADDLE) && SUBLIFT_HAS_PADDLE

#include <onnxruntime_cxx_api.h>

bool is_paddle_available() noexcept {
  return true;
}

struct PaddleOcrEngine::Impl {
  PaddleOcrOptions options;
  Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "SubLiftPaddle"};
  Ort::SessionOptions session_options;
  // Det and Rec sessions initialized when model files are loaded
  std::unique_ptr<Ort::Session> det_session;
  std::unique_ptr<Ort::Session> rec_session;
  std::vector<std::string> dictionary;

  explicit Impl(PaddleOcrOptions opts) : options(std::move(opts)) {
    session_options.SetIntraOpNumThreads(1);
    session_options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
  }
};

PaddleOcrEngine::PaddleOcrEngine(PaddleOcrOptions options)
    : impl_(std::make_unique<Impl>(std::move(options))) {}

PaddleOcrEngine::~PaddleOcrEngine() = default;

PaddleOcrEngine::PaddleOcrEngine(PaddleOcrEngine&&) noexcept = default;
PaddleOcrEngine& PaddleOcrEngine::operator=(PaddleOcrEngine&&) noexcept = default;

OcrResult PaddleOcrEngine::recognize(const ImageView& image) {
  if (image.width() <= 0 || image.height() <= 0) {
    return OcrResult::from_lines({});
  }

  try {
    // If sessions are not initialized, return empty OcrResult (or model load error in 06704)
    if (!impl_ || !impl_->det_session || !impl_->rec_session) {
      return OcrResult::from_lines({});
    }

    std::vector<OcrLine> lines;
    // PP-OCR Det -> Bbox -> Rec CTC -> OcrLine
    sort_ocr_lines(lines);
    return OcrResult::from_lines(lines);
  } catch (const Ort::Exception& e) {
    throw std::runtime_error(std::string("PaddleOCR ONNX Runtime 推理故障: ") + e.what());
  } catch (const std::exception& e) {
    throw std::runtime_error(std::string("PaddleOCR 推理故障: ") + e.what());
  }
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
