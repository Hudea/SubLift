#pragma once

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "sublift/models.hpp"
#include "sublift/ocr.hpp"

namespace sublift {

/// Default Vision recognition languages (zh-Hans, en-US)
inline constexpr std::array<std::string_view, 2> kDefaultVisionLanguages = {
    "zh-Hans",
    "en-US",
};

/// Check if Apple Vision API is available on current platform & build config.
[[nodiscard]] bool is_vision_available() noexcept;

/// Banker's Rounding (round half to even) matching Python 3 round().
[[nodiscard]] inline std::int32_t bankers_round(double v) noexcept {
  const double r = std::round(v);
  const double diff = r - v;
  if (diff == 0.5 || diff == -0.5) {
    const auto int_r = static_cast<std::int64_t>(r);
    if (int_r % 2 != 0) {
      return static_cast<std::int32_t>(int_r - (diff > 0 ? 1 : -1));
    }
  }
  return static_cast<std::int32_t>(r);
}

/// Clamp pixel box into [0, image_width] x [0, image_height].
/// If image_width <= 0 or image_height <= 0, returns OcrCropBox{0, 0, 0, 0}.
[[nodiscard]] OcrCropBox clamp_ocr_box(
    std::int32_t x, std::int32_t y,
    std::int32_t w, std::int32_t h,
    std::int32_t image_width, std::int32_t image_height) noexcept;

/// Convert Vision normalized box (origin bottom-left, [0..1]) into pixel OcrCropBox
/// (origin top-left, [0..W], [0..H]) and clamp into image boundary using Banker's rounding.
[[nodiscard]] OcrCropBox vision_normalized_box_to_pixel(
    double nx, double ny, double nw, double nh,
    std::int32_t image_width, std::int32_t image_height) noexcept;

/// Stable sort OCR recognition lines (primary key: y ascending, secondary key: x ascending).
void sort_ocr_lines(std::span<OcrLine> lines);

/// True if `text` is empty after Python-like `str.strip()` (ASCII + common Unicode Zs,
/// including U+3000 ideographic space). Used to drop blank Vision lines before from_lines.
[[nodiscard]] bool is_ocr_text_blank(std::string_view text) noexcept;

/// Apple Vision OCR engine implementation (macOS ObjC++ wrapper).
/// When compiled with SUBLIFT_ENABLE_VISION=OFF, constructor throws std::runtime_error.
///
/// Threading: **not thread-safe**. Concurrent `recognize` calls are unsupported;
/// callers must serialize access (one in-flight recognize per process is the safe default).
///
/// Pixel path: product/parity path is **RGB24** tight-or-padded stride (Oracle PIL→RGB).
/// BGR24 / Gray8 are best-effort conversions to DeviceRGB for integration convenience only.
class VisionOcrEngine final : public IOcrEngine {
public:
  explicit VisionOcrEngine(
      std::vector<std::string> recognition_languages = {
          std::string{kDefaultVisionLanguages[0]},
          std::string{kDefaultVisionLanguages[1]},
      });
  ~VisionOcrEngine() override;

  VisionOcrEngine(const VisionOcrEngine&) = delete;
  VisionOcrEngine& operator=(const VisionOcrEngine&) = delete;
  VisionOcrEngine(VisionOcrEngine&&) noexcept;
  VisionOcrEngine& operator=(VisionOcrEngine&&) noexcept;

  /// Run Vision OCR. Empty image / CGImage failure / perform failure / no observations
  /// → empty OcrResult. Unexpected C++ exceptions propagate (not swallowed).
  [[nodiscard]] OcrResult recognize(const ImageView& image) override;
  [[nodiscard]] const std::vector<std::string>& recognition_languages() const noexcept;

private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace sublift
