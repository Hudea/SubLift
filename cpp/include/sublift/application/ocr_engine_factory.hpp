#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "sublift/ocr.hpp"

namespace sublift::application {

/// Abstract factory interface for creating OCR engines and reporting capabilities.
/// This decouples the application layer from specific engine adapter implementations.
class IOcrEngineFactory {
 public:
  virtual ~IOcrEngineFactory() = default;

  /// Returns the name of the engine this factory is bound to.
  [[nodiscard]] virtual const std::string& bound_engine() const noexcept = 0;

  /// Returns a list of supported engine names.
  [[nodiscard]] virtual std::vector<std::string> supported_engines() const = 0;

  /// Returns a list of capabilities supported by the bound engine.
  [[nodiscard]] virtual std::vector<std::string> capabilities() const = 0;

  /// Validates whether the requested engine is supported.
  /// Returns std::nullopt if valid, else an error message.
  [[nodiscard]] virtual std::optional<std::string> validate_engine(const std::string& requested_engine) const = 0;

  /// Creates a new OCR engine instance.
  [[nodiscard]] virtual std::unique_ptr<sublift::IOcrEngine> create_engine(
      const std::string& mock_text = "mock_ocr", double mock_confidence = 1.0) const = 0;
};

}  // namespace sublift::application
