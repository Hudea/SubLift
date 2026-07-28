#pragma once

#include <mutex>
#include "bridge.hpp"
#include "engine_factory.hpp"

namespace sublift::worker {

class WorkerConnection {
 public:
  WorkerConnection(int client_fd, EngineFactory engine_factory);
  ~WorkerConnection();

  WorkerConnection(const WorkerConnection&) = delete;
  WorkerConnection& operator=(const WorkerConnection&) = delete;

  /// Run blocking socket reading loop until peer disconnects or sends bye.
  void run();

  /// Thread-safe send framed message to client_fd.
  void send_message(const ipc::Message& msg);

 private:
  int fd_{-1};
  EngineFactory engine_factory_;
  std::mutex write_mutex_;
  BridgeHandler bridge_handler_;
};

}  // namespace sublift::worker
