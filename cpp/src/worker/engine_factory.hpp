#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "sublift/application/ocr_engine_factory.hpp"
#include "sublift/ocr.hpp"
#include "sublift/ocr_execution.hpp"

namespace sublift::worker {

class EngineFactory final : public sublift::application::IOcrEngineFactory {
 public:
  explicit EngineFactory(std::string bound_engine = "mock",
                         sublift::PaddleExecutionConfig paddle_execution = {});
  ~EngineFactory() override = default;

  [[nodiscard]] const std::string& bound_engine() const noexcept override { return bound_engine_; }
  [[nodiscard]] std::vector<std::string> supported_engines() const override;
  [[nodiscard]] std::vector<std::string> capabilities() const override;

  /// 绑定的 Paddle 执行后端（ADR-0041）；默认 CPU。
  [[nodiscard]] const sublift::PaddleExecutionConfig& paddle_execution() const noexcept {
    return paddle_execution_;
  }

  [[nodiscard]] std::optional<std::string> validate_engine(const std::string& requested_engine) const override;

  [[nodiscard]] std::unique_ptr<sublift::IOcrEngine> create_engine(
      const std::string& mock_text = "mock_ocr", double mock_confidence = 1.0) const override;

 private:
  std::string bound_engine_;
  sublift::PaddleExecutionConfig paddle_execution_;
};

}  // namespace sublift::worker
