#pragma once

#include <memory>
#include <mutex>

#include "bridge.hpp"
#include "sublift/application/detector_factory.hpp"
#include "sublift/application/ocr_engine_factory.hpp"
#include "sublift/application/path_media_services.hpp"

namespace sublift::worker {

class WorkerConnection {
 public:
  WorkerConnection(int client_fd,
                   std::unique_ptr<sublift::application::IOcrEngineFactory> engine_factory,
                   std::unique_ptr<sublift::application::IPathMediaServices> path_media,
                   std::unique_ptr<sublift::application::IDetectorFactory> detector_factory);
  ~WorkerConnection();

  WorkerConnection(const WorkerConnection&) = delete;
  WorkerConnection& operator=(const WorkerConnection&) = delete;

  void run();
  void send_message(const ipc::Message& msg);

 private:
  int fd_{-1};
  std::mutex write_mutex_;
  BridgeHandler bridge_handler_;
};

}  // namespace sublift::worker
