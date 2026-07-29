#include "sublift/paddle.hpp"

#include <stdexcept>
#include <vector>

#include "paddle_models.hpp"
#include "ppocr_ctc.hpp"

namespace sublift {

#if defined(SUBLIFT_HAS_PADDLE) && SUBLIFT_HAS_PADDLE

#include <onnxruntime_cxx_api.h>

bool is_paddle_available() noexcept {
  return true;
}

struct PaddleOcrEngine::Impl {
  PaddleOcrOptions options;
  paddle_detail::ModelType model_type;
  std::filesystem::path model_dir;
  paddle_detail::ModelPaths model_paths;

  Ort::Env env{ORT_LOGGING_LEVEL_WARNING, "SubLiftPaddle"};
  Ort::SessionOptions session_options;
  std::unique_ptr<Ort::Session> det_session;
  std::unique_ptr<Ort::Session> rec_session;
  std::vector<std::string> dictionary;

  explicit Impl(PaddleOcrOptions opts)
      : options(std::move(opts)),
        model_type(paddle_detail::parse_model_type(options.model_type)),
        model_dir(paddle_detail::resolve_model_dir(options.model_root_dir)),
        model_paths(paddle_detail::get_expected_model_paths(model_dir, model_type)) {
    session_options.SetIntraOpNumThreads(1);
    session_options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);

    std::string err_msg;
    if (!paddle_detail::validate_model_paths(model_paths, &err_msg)) {
      // Missing model files in C++ native engine -> throw runtime_error with recovery guide
      throw std::runtime_error("PaddleOCR 不可用：" + err_msg +
                               "；请先通过 Python uv sync --extra paddle 运行或设置 "
                               "SUBLIFT_PADDLE_MODEL_DIR 环境变量。");
    }
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
    if (!impl_ || !impl_->det_session || !impl_->rec_session) {
      return OcrResult::from_lines({});
    }

    std::vector<OcrLine> lines;
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

PaddleOcrEngine::PaddleOcrEngine(PaddleOcrOptions options) {
  // Validate model_type first
  (void)paddle_detail::parse_model_type(options.model_type);
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
