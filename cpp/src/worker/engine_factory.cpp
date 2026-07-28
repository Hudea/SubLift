#include "engine_factory.hpp"

#include <stdexcept>
#include "sublift/mock_ocr.hpp"
#include "sublift/vision.hpp"

namespace sublift::worker {

EngineFactory::EngineFactory(std::string bound_engine)
    : bound_engine_(std::move(bound_engine)) {}

std::vector<std::string> EngineFactory::supported_engines() const {
#if !SUBLIFT_HAS_OPENCV
  // Mock/Vision can be constructed in isolation, but neither can produce a
  // subtitle job without the signature/changepoint Pipeline.  Do not let a
  // protocol-only diagnostic Worker advertise a runnable engine.
  return {};
#else
  // A Worker process owns exactly one engine selected at launch.  Advertising
  // every engine installed on the machine would make the handshake claim that
  // a later start_job can be accepted when validate_engine() must reject it.
  if (bound_engine_ == "vision" && !sublift::is_vision_available()) {
    return {};
  }
  if (bound_engine_ == "mock" || bound_engine_ == "vision") {
    return {bound_engine_};
  }
  return {};
#endif
}

std::vector<std::string> EngineFactory::capabilities() const {
#if !SUBLIFT_HAS_OPENCV
  return {};
#else
  return {"path_mode", "frame_mode", "push_entry", "cancel"};
#endif
}

std::optional<std::string> EngineFactory::validate_engine(const std::string& requested_engine) const {
#if !SUBLIFT_HAS_OPENCV
  (void)requested_engine;
  return "C++ worker 缺少 OpenCV 签名流水线，不能处理任务；请以 SUBLIFT_ENABLE_OPENCV=ON 重建";
#else
  if (requested_engine == "paddle") {
    return "paddle 引擎不支持在 C++ worker 中运行，请选用 Python worker";
  }
  if (requested_engine != bound_engine_) {
    return "engine 不匹配: server 使用 '" + bound_engine_ + "'，start_job 请求 '" + requested_engine + "'";
  }
  if (requested_engine == "vision" && !sublift::is_vision_available()) {
    return "Vision OCR 引擎在当前环境不可用（SUBLIFT_ENABLE_VISION=OFF 或系统版本不支持）";
  }
  if (requested_engine != "mock" && requested_engine != "vision") {
    return "不支持的 OCR 引擎: '" + requested_engine + "'";
  }
  return std::nullopt;
#endif
}

std::unique_ptr<sublift::IOcrEngine> EngineFactory::create_engine(
    const std::string& mock_text, double mock_confidence) const {
#if !SUBLIFT_HAS_OPENCV
  (void)mock_text;
  (void)mock_confidence;
  throw std::runtime_error(
      "C++ worker was built without the OpenCV-backed signature pipeline");
#else
  if (bound_engine_ == "vision") {
    if (!sublift::is_vision_available()) {
      throw std::runtime_error("Apple Vision OCR is not available at runtime");
    }
    return std::make_unique<sublift::VisionOcrEngine>();
  }
  if (bound_engine_ == "mock") {
    return std::make_unique<sublift::MockOcrEngine>(mock_text, mock_confidence);
  }
  throw std::runtime_error("Unknown bound_engine: " + bound_engine_);
#endif
}

}  // namespace sublift::worker
