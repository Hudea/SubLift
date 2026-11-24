#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <sublift/core/image_view.hpp>
#include <sublift/ports/ocr_engine.hpp>

namespace sublift::adapters::apple_vision {

/// Default Vision recognition languages (zh-Hans, en-US).
inline constexpr std::array<std::string_view, 2> kDefaultVisionLanguages = {
    "zh-Hans",
    "en-US",
};

struct AppleVisionOptions {
  std::vector<std::string> recognition_languages{
      std::string{kDefaultVisionLanguages[0]},
      std::string{kDefaultVisionLanguages[1]},
  };

  friend bool operator==(const AppleVisionOptions&, const AppleVisionOptions&) = default;
};

struct AppleVisionCapabilities {
  bool available{false};
  std::string detail{};
};

struct AppleVisionRuntimeStats {
  std::uint64_t recognize_calls{0};
  std::uint64_t recognized_lines{0};
};

/// Check if Apple Vision API is available on current platform & build config.
[[nodiscard]] bool is_vision_available() noexcept;

/// Apple Vision OCR engine implementation (macOS ObjC++ wrapper).
/// When compiled with SUBLIFT_ENABLE_VISION=0, constructor throws std::runtime_error.
///
/// Pixel path: supports PixelFormat::RGB24 and PixelFormat::Gray8.
class AppleVisionOcrEngine final : public IOcrEngine {
 public:
  explicit AppleVisionOcrEngine(AppleVisionOptions options = {});
  explicit AppleVisionOcrEngine(std::vector<std::string> recognition_languages);
  ~AppleVisionOcrEngine() override;

  AppleVisionOcrEngine(const AppleVisionOcrEngine&) = delete;
  AppleVisionOcrEngine& operator=(const AppleVisionOcrEngine&) = delete;
  AppleVisionOcrEngine(AppleVisionOcrEngine&&) noexcept;
  AppleVisionOcrEngine& operator=(AppleVisionOcrEngine&&) noexcept;

  [[nodiscard]] OcrResult recognize(const ImageView& image) override;
  [[nodiscard]] std::string engine_name() const override;

  [[nodiscard]] const std::vector<std::string>& recognition_languages() const noexcept;
  [[nodiscard]] AppleVisionRuntimeStats runtime_stats() const noexcept;
  [[nodiscard]] static AppleVisionCapabilities probe_capabilities() noexcept;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace sublift::adapters::apple_vision
