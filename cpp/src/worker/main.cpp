#include <cstddef>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <csignal>
#include <iostream>
#include <string>
#include <string_view>

#include "connection.hpp"
#include "engine_factory.hpp"

static void print_usage(const char* prog) {
  std::cout << "Usage: " << prog << " --socket <socket_path> [--engine mock|vision]\n";
}

int main(int argc, char* argv[]) {
  // Ignore SIGPIPE to avoid process kill on client disconnect
  ::signal(SIGPIPE, SIG_IGN);

  std::string socket_path;
  std::string engine = "mock";

  for (int i = 1; i < argc; ++i) {
    std::string_view arg = argv[i];
    if ((arg == "-s" || arg == "--socket") && i + 1 < argc) {
      socket_path = argv[++i];
    } else if ((arg == "-e" || arg == "--engine") && i + 1 < argc) {
      engine = argv[++i];
    } else if (arg == "-v" || arg == "--version") {
      std::cout << "sublift_worker 1.0 (Phase 6.5 C++ Core IPC)\n";
      return 0;
    } else if (arg == "-h" || arg == "--help") {
      print_usage(argv[0]);
      return 0;
    }
  }

  if (socket_path.empty()) {
    std::cerr << "Error: --socket parameter is required.\n";
    print_usage(argv[0]);
    return 1;
  }

  sublift::worker::EngineFactory engine_factory(engine);

  if (socket_path.size() >= sizeof(sockaddr_un::sun_path)) {
    std::cerr << "Error: --socket path is too long for AF_UNIX (max "
              << sizeof(sockaddr_un::sun_path) - 1 << " bytes).\n";
    return 1;
  }

  // Clean up stale socket file if it exists
  ::unlink(socket_path.c_str());

  int server_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
  if (server_fd < 0) {
    std::cerr << "Error: Failed to create UNIX socket: " << ::strerror(errno) << "\n";
    return 1;
  }

  struct sockaddr_un addr {};
  addr.sun_family = AF_UNIX;
  std::memcpy(addr.sun_path, socket_path.c_str(), socket_path.size() + 1);

  const auto addr_len = static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + socket_path.size() + 1);
  if (::bind(server_fd, reinterpret_cast<struct sockaddr*>(&addr), addr_len) < 0) {
    std::cerr << "Error: Failed to bind socket to " << socket_path << ": " << ::strerror(errno) << "\n";
    ::close(server_fd);
    return 1;
  }

  if (::listen(server_fd, 5) < 0) {
    std::cerr << "Error: Failed to listen on socket: " << ::strerror(errno) << "\n";
    ::close(server_fd);
    ::unlink(socket_path.c_str());
    return 1;
  }

  std::cout << "sublift_worker listening on " << socket_path << " (engine: " << engine << ")\n";

  while (true) {
    int client_fd = ::accept(server_fd, nullptr, nullptr);
    if (client_fd < 0) {
      if (errno == EINTR) continue;
      std::cerr << "Error: accept failed: " << ::strerror(errno) << "\n";
      break;
    }

    sublift::worker::WorkerConnection conn(client_fd, engine_factory);
    conn.run();
  }

  ::close(server_fd);
  ::unlink(socket_path.c_str());
  return 0;
}
