#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "sublift/ocr.hpp"

namespace sublift::worker {

class EngineFactory {
 public:
  explicit EngineFactory(std::string bound_engine = "mock");

  [[nodiscard]] const std::string& bound_engine() const noexcept { return bound_engine_; }
  [[nodiscard]] std::vector<std::string> supported_engines() const;
  [[nodiscard]] std::vector<std::string> capabilities() const;

  /// Check whether the requested engine matches bound_engine_.
  /// Returns std::nullopt if matching; else human-readable error description.
  [[nodiscard]] std::optional<std::string> validate_engine(const std::string& requested_engine) const;

  /// Create an IOcrEngine instance corresponding to bound_engine_.
  [[nodiscard]] std::unique_ptr<sublift::IOcrEngine> create_engine(
      const std::string& mock_text = "mock_ocr", double mock_confidence = 1.0) const;

 private:
  std::string bound_engine_;
};

}  // namespace sublift::worker
