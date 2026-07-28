#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "engine_factory.hpp"
#include "protocol.hpp"
#include "sublift/bottom_crop_detector.hpp"
#include "sublift/detector.hpp"
#include "sublift/ffmpeg.hpp"
#include "sublift/fixed_detector.hpp"
#include "sublift/pipeline.hpp"
#include "sublift/roi_passthrough_detector.hpp"

namespace sublift::worker {

using PushCallback = std::function<void(const ipc::Message&)>;

class BridgeHandler {
 public:
  explicit BridgeHandler(EngineFactory engine_factory);
  ~BridgeHandler();

  BridgeHandler(const BridgeHandler&) = delete;
  BridgeHandler& operator=(const BridgeHandler&) = delete;

  /// Process an incoming IPC message. Returns immediate response if sync;
  /// for async long-running jobs, pushes messages via push_cb.
  [[nodiscard]] std::optional<ipc::Message> handle(const ipc::Message& msg, PushCallback push_cb);

  /// Signal running job cancel and wait worker thread exit.
  void cancel_job();

 private:
  [[nodiscard]] ipc::Message handle_hello(const ipc::HelloMsg& msg);
  [[nodiscard]] std::optional<ipc::Message> handle_start_job(const ipc::StartJobMsg& msg, PushCallback push_cb);
  [[nodiscard]] std::optional<ipc::Message> handle_frame(const ipc::FrameMsg& msg, PushCallback push_cb);
  [[nodiscard]] std::optional<ipc::Message> handle_finalize(const ipc::FinalizeMsg& msg, PushCallback push_cb);
  [[nodiscard]] ipc::Message handle_cancel(const ipc::CancelJobMsg& msg);

  void run_path_mode(ipc::StartJobMsg msg, PushCallback push_cb);

  /// Drop extractor/pipeline/detector/OCR under state_mutex_. Caller sets flags.
  void release_job_resources();

  EngineFactory engine_factory_;
  std::atomic<bool> is_job_running_{false};
  std::atomic<bool> is_path_mode_{false};
  std::atomic<bool> cancelled_{false};
  std::string current_video_id_;

  std::mutex state_mutex_;
  std::thread worker_thread_;
  std::shared_ptr<sublift::ffmpeg::FfmpegExtractor> current_extractor_;
  std::shared_ptr<sublift::Pipeline> current_pipeline_;

  // Detector and OCR for the active job (path or frame). Must outlive Pipeline
  // (Pipeline holds non-owning refs; owning injection deferred).
  std::unique_ptr<sublift::IDetector> job_detector_;
  std::unique_ptr<sublift::IOcrEngine> job_ocr_engine_;
};

}  // namespace sublift::worker
