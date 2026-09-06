#include <cctype>
#include <csignal>
#include <cstdlib>
#include <filesystem>
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
            << "      --static-dir <path>  Directory to serve static web assets from\n"
            << "      --cors-origin <o>    Allow-Origin for cross-origin use (default: off)\n"
            << "      --media-dir <path>   Media workspace root; also locks the workspace\n"
            << "      --config-file <path> Override workspace config file path\n"
            << "\n"
            << "Environment:\n"
            << "  SUBLIFT_HOST / SUBLIFT_PORT / SUBLIFT_STATIC_DIR / SUBLIFT_CORS_ORIGIN\n"
            << "  SUBLIFT_MEDIA_DIR        Media workspace root (locks the workspace)\n"
            << "  SUBLIFT_ACCESS_TOKEN     Require 'Authorization: Bearer <token>' on /api/*\n"
            << "  SUBLIFT_LOCK_WORKSPACE   Force-lock the workspace (1/true/yes/on)\n"
            << "\n"
            << "Binding a non-loopback address requires a configured and locked media root,\n"
            << "and an available Paddle OCR engine (no silent mock fallback).\n";
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

  std::string cors_origin = "";
  if (const char* env_cors = std::getenv("SUBLIFT_CORS_ORIGIN"); env_cors && *env_cors) {
    cors_origin = env_cors;
  }

  std::string media_dir = "";
  if (const char* env_media = std::getenv("SUBLIFT_MEDIA_DIR"); env_media && *env_media) {
    media_dir = env_media;
  } else if (const char* env_root = std::getenv("SUBLIFT_ALLOWED_MEDIA_ROOT"); env_root && *env_root) {
    media_dir = env_root;
  }

  std::string config_file = "";
  if (const char* env_cfg = std::getenv("SUBLIFT_CONFIG_FILE"); env_cfg && *env_cfg) {
    config_file = env_cfg;
  }

  std::string access_token = "";
  if (const char* env_token = std::getenv("SUBLIFT_ACCESS_TOKEN"); env_token && *env_token) {
    access_token = env_token;
  }

  // 工作区锁定：启动期注入媒体根即锁定；SUBLIFT_LOCK_WORKSPACE 可显式强制锁定。
  bool workspace_locked = !media_dir.empty();
  if (const char* env_lock = std::getenv("SUBLIFT_LOCK_WORKSPACE"); env_lock && *env_lock) {
    std::string flag;
    for (const char* p = env_lock; *p != '\0'; ++p) {
      flag.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(*p))));
    }
    if (flag == "1" || flag == "true" || flag == "yes" || flag == "on") {
      workspace_locked = true;
    }
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
    } else if (arg == "--cors-origin" && i + 1 < argc) {
      cors_origin = argv[++i];
    } else if (arg == "--media-dir" && i + 1 < argc) {
      media_dir = argv[++i];
      workspace_locked = true;
    } else if (arg == "--config-file" && i + 1 < argc) {
      config_file = argv[++i];
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
      .cors_origin = cors_origin,
      .media_dir = media_dir,
      .config_file = config_file,
      .access_token = access_token,
      .workspace_locked = workspace_locked,
  };

  g_server = std::make_unique<sublift::server::HttpServer>(std::move(config));

  const bool media_configured = g_server->workspace_manager() &&
                                g_server->workspace_manager()->get_media_dir().has_value();
  {
    sublift::server::ServerConfig exposure_check;
    exposure_check.host = host;
    exposure_check.workspace_locked = workspace_locked;
    exposure_check.access_token = access_token;
    if (const std::string err =
            sublift::server::validate_remote_exposure(exposure_check, media_configured);
        !err.empty()) {
      std::cerr << "Error: " << err << "\n";
      return 1;
    }
  }

  std::signal(SIGINT, handle_signal);
  std::signal(SIGTERM, handle_signal);

  auto sys_info = sublift::server::collect_system_info();
  bool paddle_available = false;
  for (const auto& eng : sys_info.engines) {
    if (eng.name == "paddle" && eng.available) {
      paddle_available = true;
      break;
    }
  }
  {
    sublift::server::ServerConfig paddle_check;
    paddle_check.host = host;
    if (const std::string err =
            sublift::server::validate_remote_paddle(paddle_check, paddle_available);
        !err.empty()) {
      std::cerr << "Error: " << err << "\n";
      return 1;
    }
  }

  std::cout << "====================================================\n";
  std::cout << " SubLift Native Web Server v" << sublift::version() << "\n";
  std::cout << "====================================================\n";
  std::cout << " Listening on: http://" << host << ":" << port << "\n";
  if (!static_dir.empty()) {
    std::cout << " Static dir:   " << static_dir << "\n";
  }
  std::cout << " CORS:         " << (cors_origin.empty() ? "off (same-origin only)" : cors_origin) << "\n";
  std::cout << " Workspace:    " << (media_configured ? "configured" : "unset")
            << (workspace_locked ? " (locked)" : "") << "\n";
  std::cout << " Access token: " << (access_token.empty() ? "off (trusted LAN only)" : "required") << "\n";
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
