#include "bridge.hpp"

#include <type_traits>
#include <utility>

namespace sublift::worker {

namespace {

constexpr const char* kPipelineUnavailable =
    "C++ worker 缺少 OpenCV 签名流水线，不能处理任务；请以 SUBLIFT_ENABLE_OPENCV=ON 重建";

}  // namespace

BridgeHandler::BridgeHandler(EngineFactory engine_factory)
    : engine_factory_(std::move(engine_factory)) {}

BridgeHandler::~BridgeHandler() {
  cancel_job();
}

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

std::optional<ipc::Message> BridgeHandler::handle(const ipc::Message& msg, PushCallback push_cb) {
  return std::visit(
      [this, &push_cb](auto&& arg) -> std::optional<ipc::Message> {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, ipc::HelloMsg>) {
          return handle_hello(arg);
        } else if constexpr (std::is_same_v<T, ipc::StartJobMsg>) {
          return handle_start_job(arg, push_cb);
        } else if constexpr (std::is_same_v<T, ipc::FrameMsg>) {
          return handle_frame(arg, push_cb);
        } else if constexpr (std::is_same_v<T, ipc::FinalizeMsg>) {
          return handle_finalize(arg, push_cb);
        } else if constexpr (std::is_same_v<T, ipc::CancelJobMsg>) {
          return handle_cancel(arg);
        } else {
          return std::nullopt;
        }
      },
      msg);
}

ipc::Message BridgeHandler::handle_hello(const ipc::HelloMsg& /*msg*/) {
  return ipc::ByeMsg{
      .protocol_version = 1,
      .runtime = "cpp",
      .engines = engine_factory_.supported_engines(),
      .capabilities = engine_factory_.capabilities(),
  };
}

std::optional<ipc::Message> BridgeHandler::handle_start_job(const ipc::StartJobMsg& msg,
                                                            PushCallback /*push_cb*/) {
  return ipc::DoneMsg{
      .video_id = msg.video_id,
      .ok = false,
      .error = kPipelineUnavailable,
  };
}

std::optional<ipc::Message> BridgeHandler::handle_frame(const ipc::FrameMsg& /*msg*/,
                                                        PushCallback /*push_cb*/) {
  return ipc::ErrorMsg{.message = kPipelineUnavailable};
}

std::optional<ipc::Message> BridgeHandler::handle_finalize(const ipc::FinalizeMsg& msg,
                                                           PushCallback /*push_cb*/) {
  return ipc::DoneMsg{
      .video_id = msg.video_id,
      .ok = false,
      .error = kPipelineUnavailable,
  };
}

ipc::Message BridgeHandler::handle_cancel(const ipc::CancelJobMsg& msg) {
  cancel_job();
  return ipc::DoneMsg{
      .video_id = msg.video_id,
      .ok = false,
      .error = "cancelled",
  };
}

void BridgeHandler::run_path_mode(ipc::StartJobMsg msg, PushCallback push_cb) {
  push_cb(ipc::DoneMsg{
      .video_id = std::move(msg.video_id),
      .ok = false,
      .error = kPipelineUnavailable,
  });
}

}  // namespace sublift::worker
