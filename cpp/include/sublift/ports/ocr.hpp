#pragma once

#include "sublift/image.hpp"
#include "sublift/models.hpp"

namespace sublift {

/// OCR engine abstraction. Matches the Python `OcrEngine` Protocol in
/// `src/sublift/ocr/base.py`.
///
/// The public boundary takes a non-owning `ImageView` (not cv::Mat / PIL), per
/// architecture.md §3: no third-party image type leaks across sublift_core's
/// API. Concrete engines (Vision in 6.4, Paddle worker later) implement this;
/// `MockOcrEngine` (see mock_ocr.hpp) covers 6.2 parity.
///
/// Not thread-safe by contract: callers must serialize `recognize` on a single
/// engine instance (same as the Python pipeline).
///
/// Non-copyable polymorphic base (prevents slicing). Move is allowed so
/// concrete engines/test doubles can be returned by value.
struct IOcrEngine {
  IOcrEngine() = default;
  IOcrEngine(const IOcrEngine&) = delete;
  IOcrEngine& operator=(const IOcrEngine&) = delete;
  IOcrEngine(IOcrEngine&&) = default;
  IOcrEngine& operator=(IOcrEngine&&) = default;
  virtual ~IOcrEngine() = default;

  /// Recognize text in `image`. The view is borrowed for the call only; the
  /// engine must not retain it beyond the call.
  [[nodiscard]] virtual OcrResult recognize(const ImageView& image) = 0;
};

}  // namespace sublift
