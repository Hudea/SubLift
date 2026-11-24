#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include <sublift/core/subtitle.hpp>
#include <sublift/engine/config.hpp>
#include <sublift/engine/observer.hpp>
#include <sublift/ports/ocr_engine.hpp>
#include <sublift/timing/segment.hpp>

namespace sublift {

/**
 * @brief Asynchronously executes OCR recognition and multi-frame consensus on closed segments.
 *
 * Decouples OCR execution from the state machine consumer thread using a dedicated worker thread.
 * Guarantees serialized calls to IOcrEngine::recognize(), notifies observer on subtitle entry,
 * and immediately destroys Segment pixel buffers once OCR is complete to ensure bounded memory.
 */
class SegmentRecognizer {
 public:
  SegmentRecognizer(IOcrEngine& ocr_engine,
                    const ExtractionConfig& config,
                    IExtractionObserver* observer = nullptr);
  ~SegmentRecognizer();

  SegmentRecognizer(const SegmentRecognizer&) = delete;
  SegmentRecognizer& operator=(const SegmentRecognizer&) = delete;
  SegmentRecognizer(SegmentRecognizer&&) noexcept;
  SegmentRecognizer& operator=(SegmentRecognizer&&) noexcept;

  /// Set or update the observer for subtitle entry callbacks.
  void set_observer(IExtractionObserver* observer);

  /// Submit closed segment for asynchronous OCR recognition.
  void submit(timing::Segment segment, std::uint64_t entry_id);

  /// Drain all pending OCR tasks, waiting until all submissions have completed.
  /// Rethrows any exceptions that occurred in the worker thread.
  void drain();

  /// Cancel all pending OCR tasks and signal worker to stop.
  void cancel();

  /// Retrieve produced raw subtitle entries in order of segment submission.
  [[nodiscard]] std::vector<SubtitleEntry> take_raw_entries();

  /// Get total count of OCR calls performed by this recognizer.
  [[nodiscard]] std::uint64_t ocr_calls_count() const noexcept;

  /// Check and rethrow any exceptions captured during recognition.
  void check_and_rethrow();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace sublift
