#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "sublift/application/detector_factory.hpp"
#include "sublift/application/ocr_engine_factory.hpp"
#include "sublift/application/path_media_services.hpp"
#include "sublift/detector.hpp"
#include "sublift/ocr.hpp"
#include "sublift/protocol/protocol.hpp"

namespace sublift {
class Pipeline;
}

namespace sublift::worker {

using PushCallback = std::function<void(const ipc::Message&)>;

class BridgeHandler {
 public:
  BridgeHandler(std::unique_ptr<sublift::application::IOcrEngineFactory> engine_factory,
                std::unique_ptr<sublift::application::IPathMediaServices> path_media,
                std::unique_ptr<sublift::application::IDetectorFactory> detector_factory);
  ~BridgeHandler();

  BridgeHandler(const BridgeHandler&) = delete;
  BridgeHandler& operator=(const BridgeHandler&) = delete;

  [[nodiscard]] std::optional<ipc::Message> handle(const ipc::Message& msg, PushCallback push_cb);

  void cancel_job();

 private:
  [[nodiscard]] ipc::Message handle_hello(const ipc::HelloMsg& msg);
  [[nodiscard]] std::optional<ipc::Message> handle_start_job(const ipc::StartJobMsg& msg,
                                                             PushCallback push_cb);
  [[nodiscard]] std::optional<ipc::Message> handle_frame(const ipc::FrameMsg& msg,
                                                         PushCallback push_cb);
  [[nodiscard]] std::optional<ipc::Message> handle_finalize(const ipc::FinalizeMsg& msg,
                                                            PushCallback push_cb);
  [[nodiscard]] ipc::Message handle_cancel(const ipc::CancelJobMsg& msg);

  void run_path_mode(ipc::StartJobMsg msg, PushCallback push_cb);
  void release_job_resources();

  std::unique_ptr<sublift::application::IOcrEngineFactory> engine_factory_;
  std::unique_ptr<sublift::application::IPathMediaServices> path_media_;
  std::unique_ptr<sublift::application::IDetectorFactory> detector_factory_;
  std::atomic<bool> is_job_running_{false};
  std::atomic<bool> is_path_mode_{false};
  std::atomic<bool> cancelled_{false};
  std::string current_video_id_;

  std::mutex state_mutex_;
  std::thread worker_thread_;
  std::shared_ptr<sublift::application::IStreamingExtractor> current_extractor_;
  std::shared_ptr<sublift::Pipeline> current_pipeline_;

  std::unique_ptr<sublift::IDetector> job_detector_;
  std::unique_ptr<sublift::IOcrEngine> job_ocr_engine_;
};

}  // namespace sublift::worker
