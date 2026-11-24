#include "worker_app.hpp"

#include <iostream>

int main() {
  std::ios::sync_with_stdio(false);
  sublift::worker::DefaultWorkerBackend backend;
  sublift::worker::WorkerServer server(backend);
  return server.run(std::cin, std::cout);
}
