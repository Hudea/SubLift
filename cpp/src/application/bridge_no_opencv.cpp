#include "bridge.hpp"

#include <type_traits>
#include <utility>

namespace sublift::worker {

namespace {

constexpr const char* kPipelineUnavailable =
    "C++ worker 缺少 OpenCV 签名流水线，不能处理任务；请以 SUBLIFT_ENABLE_OPENCV=ON 重建";

}  // namespace

BridgeHandler::BridgeHandler(
    std::unique_ptr<sublift::application::IOcrEngineFactory> engine_factory,
    std::unique_ptr<sublift::application::IPathMediaServices> path_media)
    : engine_factory_(std::move(engine_factory)), path_media_(std::move(path_media)) {}

BridgeHandler::~BridgeHandler() { cancel_job(); }

void BridgeHandler::release_job_resources() {
  std::lock_guard<std::mutex> lock(state_mutex_);
  current_extractor_.reset();
  current_pipeline_.reset();
  job_detector_.reset();
  job_ocr_engine_.reset();
}

void BridgeHandler::cancel_job() {
  cancelled_ = true;
  if (worker_thread_.joinable()) {
    worker_thread_.join();
  }
  release_job_resources();
  is_job_running_ = false;
  is_path_mode_ = false;
}

std::optional<ipc::Message> BridgeHandler::handle(const ipc::Message& msg, PushCallback) {
  return std::visit(
      [this](auto&& arg) -> std::optional<ipc::Message> {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, ipc::HelloMsg>) {
          return handle_hello(arg);
        } else if constexpr (std::is_same_v<T, ipc::StartJobMsg>) {
          return ipc::DoneMsg{
              .video_id = arg.video_id,
              .ok = false,
              .error = kPipelineUnavailable,
          };
        } else if constexpr (std::is_same_v<T, ipc::FrameMsg>) {
          return ipc::DoneMsg{
              .video_id = arg.video_id,
              .ok = false,
              .error = kPipelineUnavailable,
          };
        } else if constexpr (std::is_same_v<T, ipc::FinalizeMsg>) {
          return ipc::DoneMsg{
              .video_id = arg.video_id,
              .ok = false,
              .error = kPipelineUnavailable,
          };
        } else if constexpr (std::is_same_v<T, ipc::CancelJobMsg>) {
          return handle_cancel(arg);
        } else {
          return std::nullopt;
        }
      },
      msg);
}

ipc::Message BridgeHandler::handle_hello(const ipc::HelloMsg&) {
  return ipc::ByeMsg{
      .protocol_version = 1,
      .runtime = "cpp",
      .engines = engine_factory_ ? engine_factory_->supported_engines()
                                 : std::vector<std::string>{},
      .capabilities = engine_factory_ ? engine_factory_->capabilities()
                                      : std::vector<std::string>{},
  };
}

std::optional<ipc::Message> BridgeHandler::handle_start_job(const ipc::StartJobMsg&,
                                                            PushCallback) {
  return std::nullopt;
}

std::optional<ipc::Message> BridgeHandler::handle_frame(const ipc::FrameMsg&, PushCallback) {
  return std::nullopt;
}

std::optional<ipc::Message> BridgeHandler::handle_finalize(const ipc::FinalizeMsg&,
                                                            PushCallback) {
  return std::nullopt;
}

ipc::Message BridgeHandler::handle_cancel(const ipc::CancelJobMsg& msg) {
  cancel_job();
  return ipc::DoneMsg{.video_id = msg.video_id, .ok = true, .error = ""};
}

void BridgeHandler::run_path_mode(ipc::StartJobMsg, PushCallback) {}

}  // namespace sublift::worker
