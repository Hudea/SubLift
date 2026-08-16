#include <csignal>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "sublift/server/http_server.hpp"
#include "sublift/server/routes.hpp"
#include "sublift/version.hpp"

namespace {

std::unique_ptr<sublift::server::HttpServer> g_server;

void handle_signal(int signal) {
  if (g_server) {
    std::cout << "\n[sublift_server] Received signal " << signal << ", shutting down gracefully...\n";
    g_server->stop();
  }
}

void print_help(std::string_view prog_name) {
  std::cout << "SubLift Native Web Server v" << sublift::version() << "\n\n"
            << "Usage:\n"
            << "  " << prog_name << " [options]\n\n"
            << "Options:\n"
            << "  -h, --help               Show this help message and exit\n"
            << "  -v, --version            Show version information and exit\n"
            << "      --host <ip>          Bind address (default: 127.0.0.1)\n"
            << "  -p, --port <port>        Listen port (default: 8080)\n"
            << "      --static-dir <path>  Directory to serve static web assets from\n";
}

}  // namespace

int main(int argc, char* argv[]) {
  std::string host = "127.0.0.1";
  if (const char* env_host = std::getenv("SUBLIFT_HOST"); env_host && *env_host) {
    host = env_host;
  }

  int port = 8080;
  if (const char* env_port = std::getenv("SUBLIFT_PORT"); env_port && *env_port) {
    int p = std::atoi(env_port);
    if (p > 0 && p <= 65535) {
      port = p;
    }
  }

  std::string static_dir = "";
  if (const char* env_dir = std::getenv("SUBLIFT_STATIC_DIR"); env_dir && *env_dir) {
    static_dir = env_dir;
  }

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "-h" || arg == "--help") {
      print_help(argv[0]);
      return 0;
    }
    if (arg == "-v" || arg == "--version") {
      std::cout << "sublift_server " << sublift::version() << "\n";
      return 0;
    }
    if (arg == "--host" && i + 1 < argc) {
      host = argv[++i];
    } else if ((arg == "-p" || arg == "--port") && i + 1 < argc) {
      port = std::atoi(argv[++i]);
      if (port <= 0 || port > 65535) {
        std::cerr << "Error: invalid port number: " << argv[i] << "\n";
        return 1;
      }
    } else if (arg == "--static-dir" && i + 1 < argc) {
      static_dir = argv[++i];
    } else {
      std::cerr << "Unknown option: " << arg << "\n";
      print_help(argv[0]);
      return 1;
    }
  }

  // Auto-discover web assets if --static-dir was not explicitly provided
  if (static_dir.empty()) {
    std::vector<std::string> search_candidates = {
        "apps/web/dist",
        "../apps/web/dist",
        "../../apps/web/dist",
    };
    for (const auto& candidate : search_candidates) {
      std::error_code ec;
      if (std::filesystem::exists(candidate, ec) && std::filesystem::is_directory(candidate, ec)) {
        static_dir = candidate;
        break;
      }
    }
  }

  sublift::server::ServerConfig config{
      .host = host,
      .port = port,
      .static_dir = static_dir,
      .thread_pool_size = 8,
  };

  g_server = std::make_unique<sublift::server::HttpServer>(std::move(config));

  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  auto sys_info = sublift::server::collect_system_info();
  std::cout << "====================================================\n";
  std::cout << " SubLift Native Web Server v" << sublift::version() << "\n";
  std::cout << "====================================================\n";
  std::cout << " Listening on: http://" << host << ":" << port << "\n";
  if (!static_dir.empty()) {
    std::cout << " Static dir:   " << static_dir << "\n";
  }
  std::cout << " Available OCR Engines:\n";
  for (const auto& eng : sys_info.engines) {
    std::cout << "   - [" << (eng.available ? "x" : " ") << "] " << eng.name
              << " (" << eng.detail << ")\n";
  }
  std::cout << " FFmpeg status: " << (sys_info.ffmpeg_available ? "Ready (" + sys_info.ffmpeg_path + ")" : "Not found") << "\n";
  std::cout << "====================================================\n";

  if (!g_server->listen()) {
    std::cerr << "Error: failed to bind and listen on " << host << ":" << port << "\n";
    return 1;
  }

  std::cout << "[sublift_server] Server stopped successfully.\n";
  return 0;
}
