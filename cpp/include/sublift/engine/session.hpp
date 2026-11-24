#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <vector>
#include <sublift/core/box.hpp>
#include <sublift/core/frame.hpp>
#include <sublift/core/subtitle.hpp>
#include <sublift/engine/config.hpp>
#include <sublift/engine/observer.hpp>
#include <sublift/ports/detector.hpp>
#include <sublift/ports/frame_source.hpp>
#include <sublift/ports/ocr_engine.hpp>
#include <sublift/ports/subtitle_sink.hpp>

namespace sublift {

class ExtractionSession {
 public:
  explicit ExtractionSession(
      IFrameSource& frame_source,
      IOcrEngine& ocr_engine,
      IDetector* detector = nullptr,
      ISubtitleSink* sink = nullptr,
      ExtractionConfig config = {});

  ~ExtractionSession();

  ExtractionSession(const ExtractionSession&) = delete;
  ExtractionSession& operator=(const ExtractionSession&) = delete;
  ExtractionSession(ExtractionSession&&) = delete;
  ExtractionSession& operator=(ExtractionSession&&) = delete;

  void set_observer(IExtractionObserver* observer);

  std::vector<SubtitleEntry> process_video(
      const std::filesystem::path& video_path,
      const std::optional<SourceBox>& region_box = std::nullopt);

  void feed(Frame frame);
  std::vector<SubtitleEntry> finalize();

  void cancel();

  [[nodiscard]] bool is_running() const noexcept;
  [[nodiscard]] bool is_cancelled() const noexcept;
  [[nodiscard]] ExtractionMetrics metrics() const noexcept;
  [[nodiscard]] const ExtractionConfig& config() const noexcept;

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace sublift
