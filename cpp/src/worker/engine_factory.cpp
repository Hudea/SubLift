#include "engine_factory.hpp"

#include <stdexcept>
#include "sublift/mock_ocr.hpp"
#include "sublift/vision.hpp"

namespace sublift::worker {

EngineFactory::EngineFactory(std::string bound_engine)
    : bound_engine_(std::move(bound_engine)) {}

std::vector<std::string> EngineFactory::supported_engines() const {
  std::vector<std::string> engines;
  if (sublift::is_vision_available()) {
    engines.push_back("vision");
  }
  engines.push_back("mock");
  return engines;
}

std::vector<std::string> EngineFactory::capabilities() const {
  return {"path_mode", "frame_mode", "push_entry", "cancel"};
}

std::optional<std::string> EngineFactory::validate_engine(const std::string& requested_engine) const {
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
}

std::unique_ptr<sublift::IOcrEngine> EngineFactory::create_engine(
    const std::string& mock_text, double mock_confidence) const {
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
}

}  // namespace sublift::worker
