#pragma once

#include <cstdint>
#include <string>
#include <sublift/core/subtitle.hpp>
#include <sublift/timing/segment.hpp>

namespace sublift {

enum class ExtractionErrorCode {
  SourceReadError,
  DecoderFailed,
  OcrFailed,
  InvalidConfiguration,
  Aborted
};

struct ExtractionError {
  ExtractionErrorCode code{ExtractionErrorCode::Aborted};
  std::string message{};
  std::int64_t timestamp_ms{0};
};

struct ExtractionMetrics {
  std::uint64_t total_frames_processed{0};
  std::uint64_t total_segments_detected{0};
  std::uint64_t total_ocr_calls{0};
  std::uint64_t total_entries_produced{0};
  std::int64_t wall_time_ms{0};
  double effective_fps{0.0};

  friend bool operator==(const ExtractionMetrics&, const ExtractionMetrics&) = default;
};

class IExtractionObserver {
 public:
  virtual ~IExtractionObserver() = default;

  virtual void on_progress(std::int64_t current_ms, std::int64_t total_ms, double fps) {
    (void)current_ms;
    (void)total_ms;
    (void)fps;
  }
  virtual void on_segment_detected(const timing::Segment& segment) {
    (void)segment;
  }
  virtual void on_subtitle_entry(const SubtitleEntry& entry) {
    (void)entry;
  }
  virtual void on_error(const ExtractionError& error) {
    (void)error;
  }
  virtual void on_complete(const ExtractionMetrics& metrics) {
    (void)metrics;
  }
};

} // namespace sublift
