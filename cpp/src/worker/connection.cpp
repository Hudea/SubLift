#include "connection.hpp"

#include <unistd.h>
#include <iostream>

#include "framing.hpp"

namespace sublift::worker {

WorkerConnection::WorkerConnection(
    int client_fd,
    std::unique_ptr<sublift::application::IOcrEngineFactory> engine_factory,
    std::unique_ptr<sublift::application::IPathMediaServices> path_media,
    std::unique_ptr<sublift::application::IDetectorFactory> detector_factory)
    : fd_(client_fd),
      bridge_handler_(std::move(engine_factory), std::move(path_media),
                      std::move(detector_factory)) {}

WorkerConnection::~WorkerConnection() {
  bridge_handler_.cancel_job();
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
}

void WorkerConnection::send_message(const ipc::Message& msg) {
  if (fd_ < 0) return;
  std::string json_str = ipc::serialize_message(msg);
  std::lock_guard<std::mutex> lock(write_mutex_);
  (void)ipc::write_framed_message(fd_, json_str);
}

void WorkerConnection::run() {
  if (fd_ < 0) return;

  PushCallback push_cb = [this](const ipc::Message& msg) {
    send_message(msg);
  };

  while (true) {
    auto frame_res = ipc::read_framed_message(fd_);
    if (frame_res.status == ipc::FramingStatus::Eof) {
      // Client disconnected cleanly
      break;
    }
    if (frame_res.status != ipc::FramingStatus::Success) {
      send_message(ipc::ErrorMsg{.message = "Framing error or invalid message"});
      break;
    }

    try {
      ipc::Message msg = ipc::parse_message(frame_res.payload);
      auto resp = bridge_handler_.handle(msg, push_cb);
      if (resp.has_value()) {
        send_message(*resp);
      }
      // `bye` is a client-requested connection close, not a second handshake.
      // Do this at the connection boundary so the handler's normal destructor
      // path still cancels and joins any active job before the fd is closed.
      if (std::holds_alternative<ipc::ByeMsg>(msg)) {
        break;
      }
    } catch (const ipc::ProtocolError& pe) {
      send_message(ipc::ErrorMsg{.message = std::string("Protocol error: ") + pe.what()});
    } catch (const std::exception& e) {
      send_message(ipc::ErrorMsg{.message = std::string("Unhandled error: ") + e.what()});
    }
  }

  bridge_handler_.cancel_job();
}

}  // namespace sublift::worker
