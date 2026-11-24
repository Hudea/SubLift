#pragma once

#include <cstdint>
#include <istream>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include <sublift/ports/frame_source.hpp>
#include <sublift/ports/ocr_engine.hpp>

namespace sublift::worker {

struct CapabilitiesInfo {
  std::vector<std::string> engines;
  bool ffmpeg_available{false};
  bool ffprobe_available{false};
  std::vector<std::string> hwaccel_modes;
  std::string platform;
};

struct OcrAcquireResult {
  std::shared_ptr<IOcrEngine> engine;
  std::string error_code;
  std::string error_message;

  [[nodiscard]] bool ok() const noexcept { return engine != nullptr; }
};

/// Thread-safe proxy serializing recognize() calls on a shared IOcrEngine instance.
class LockedOcrEngineProxy final : public IOcrEngine {
 public:
  LockedOcrEngineProxy(std::shared_ptr<IOcrEngine> inner,
                       std::shared_ptr<std::mutex> mutex)
      : inner_(std::move(inner)), mutex_(std::move(mutex)) {}

  [[nodiscard]] OcrResult recognize(const ImageView& image) override;
  [[nodiscard]] std::string engine_name() const override;

 private:
  std::shared_ptr<IOcrEngine> inner_;
  std::shared_ptr<std::mutex> mutex_;
};

/// Backend dependency factory allowing production FFmpeg/Paddle/Vision or test mocks.
class IWorkerBackend {
 public:
  virtual ~IWorkerBackend() = default;

  [[nodiscard]] virtual CapabilitiesInfo query_capabilities() = 0;

  [[nodiscard]] virtual std::unique_ptr<IFrameSource> create_frame_source(
      double fps,
      const std::optional<std::string>& hwaccel,
      std::int32_t max_output_width) = 0;

  [[nodiscard]] virtual OcrAcquireResult acquire_ocr_engine(
      const std::string& engine_id,
      std::int32_t reference_width) = 0;
};

/// Production backend wiring FfmpegFrameSource and lazy hot-resident Paddle/Vision engines.
class DefaultWorkerBackend final : public IWorkerBackend {
 public:
  DefaultWorkerBackend();
  ~DefaultWorkerBackend() override;

  [[nodiscard]] CapabilitiesInfo query_capabilities() override;

  [[nodiscard]] std::unique_ptr<IFrameSource> create_frame_source(
      double fps,
      const std::optional<std::string>& hwaccel,
      std::int32_t max_output_width) override;

  [[nodiscard]] OcrAcquireResult acquire_ocr_engine(
      const std::string& engine_id,
      std::int32_t reference_width) override;

 private:
  std::mutex engines_mutex_;
  std::shared_ptr<IOcrEngine> paddle_engine_;
  std::shared_ptr<std::mutex> paddle_mutex_;
  std::shared_ptr<IOcrEngine> vision_engine_;
  std::shared_ptr<std::mutex> vision_mutex_;
};

/// Persistent stdio JSON-Lines host managing concurrent tasks and graceful cancellation.
class WorkerServer {
 public:
  explicit WorkerServer(IWorkerBackend& backend);
  ~WorkerServer();

  WorkerServer(const WorkerServer&) = delete;
  WorkerServer& operator=(const WorkerServer&) = delete;

  /// Runs the blocking line-buffered request loop until `in` reaches EOF.
  /// Cancels all active tasks and joins all worker threads before returning 0.
  int run(std::istream& in, std::ostream& out);

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace sublift::worker
