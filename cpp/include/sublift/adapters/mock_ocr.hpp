#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

#include "sublift/image.hpp"
#include "sublift/models.hpp"
#include "sublift/ocr.hpp"

namespace sublift {

/// Test-only OCR engine returning preset results. Mirrors
/// `src/sublift/ocr/mock.py` (`MockOcrEngine`).
///
/// Two modes:
///  - fixed: every `recognize` returns the same `OcrResult`. Built from
///    (text, confidence), or from `lines` via `OcrResult::from_lines` when
///    line-level results are supplied.
///  - sequence: returns `sequence[i]` on the i-th call, advancing the index.
///    An out-of-range call throws `std::out_of_range` (Python raises
///    `IndexError`) to surface test bugs rather than silently repeating.
///
/// The `ImageView` argument is ignored (same as the Python mock); it only
/// satisfies the `IOcrEngine` signature.
///
/// `call_count()` counts every successful `recognize` in **both** modes
/// (sequence index is tracked separately). Use this for zero-OCR assertions
/// on the feed path.
class MockOcrEngine final : public IOcrEngine {
 public:
  MockOcrEngine() = default;

  explicit MockOcrEngine(std::string text, double confidence = 1.0)
      : fixed_result_{OcrResult{std::move(text), confidence, {}}} {}

  /// Fixed line-level result (ignores text/confidence, uses from_lines).
  explicit MockOcrEngine(std::span<const OcrLine> lines)
      : fixed_result_{OcrResult::from_lines(lines)} {}

  /// Sequence mode: returns each result in order; out-of-range throws.
  explicit MockOcrEngine(std::vector<OcrResult> sequence)
      : sequence_{std::move(sequence)} {}

  [[nodiscard]] OcrResult recognize(const ImageView& /*image*/) override {
    if (sequence_.has_value()) {
      if (index_ >= sequence_->size()) {
        throw std::out_of_range("MockOcrEngine: sequence exhausted");
      }
      ++call_count_;
      return (*sequence_)[index_++];
    }
    ++call_count_;
    return fixed_result_;
  }

  /// Number of successful `recognize` calls so far (both fixed and sequence).
  [[nodiscard]] std::size_t call_count() const noexcept { return call_count_; }

 private:
  OcrResult fixed_result_{};
  std::optional<std::vector<OcrResult>> sequence_{};
  std::size_t index_{0};
  std::size_t call_count_{0};
};

}  // namespace sublift
