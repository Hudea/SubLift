#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <variant>
#include <vector>

#include "framing.hpp"
#include "protocol.hpp"
#include "sublift/version.hpp"

namespace fs = std::filesystem;
using namespace sublift::ipc;

namespace {

void print_version() {
  std::printf("sublift %.*s\n", static_cast<int>(sublift::version().size()),
              sublift::version().data());
}

void print_usage(const char* prog) {
  std::fprintf(stderr,
               "Usage:\n"
               "  %s                          # print version\n"
               "  %s --version\n"
               "  %s extract <video> [options]\n"
               "\n"
               "Native product CLI (Phase 6.9). Spawns sibling sublift_worker over UDS.\n"
               "Supports mock, vision, and paddle OCR engines.\n"
               "\n"
               "extract options:\n"
               "  -o, --output PATH         Output SRT (default: output.srt)\n"
               "  --fps N                   Sample rate (default: 5.0)\n"
               "  --confidence N            OCR confidence (default: 0.5)\n"
               "  --engine mock|vision|paddle  OCR engine (default: vision)\n"
               "  --script auto|cjk|latin  Subtitle script (default: auto)\n"
               "  --worker PATH             Override sublift_worker binary\n",
               prog, prog, prog);
}

[[nodiscard]] std::string format_srt_timestamp(std::int64_t ms) {
  if (ms < 0) ms = 0;
  const auto total_ms = static_cast<std::int64_t>(ms);
  const auto hours = total_ms / 3'600'000;
  const auto minutes = (total_ms % 3'600'000) / 60'000;
  const auto seconds = (total_ms % 60'000) / 1000;
  const auto millis = total_ms % 1000;
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%02lld:%02lld:%02lld,%03lld",
                static_cast<long long>(hours), static_cast<long long>(minutes),
                static_cast<long long>(seconds), static_cast<long long>(millis));
  return std::string{buf};
}

[[nodiscard]] bool is_blank_text(std::string_view text) {
  for (unsigned char c : text) {
    if (!std::isspace(c)) return false;
  }
  return true;
}

/// Format SRT; skips blank-text entries (product export hygiene, matches Python SrtExporter).
[[nodiscard]] std::pair<std::string, std::size_t> format_srt(
    const std::vector<sublift::SubtitleEntry>& entries) {
  if (entries.empty()) return {"", 0};
  std::string out;
  std::size_t idx = 1;
  for (const auto& e : entries) {
    if (is_blank_text(e.text)) continue;

    out += std::to_string(idx++);
    out += '\n';
    out += format_srt_timestamp(e.start_ms);
    out += " --> ";
    out += format_srt_timestamp(e.end_ms);
    out += '\n';
    out += e.text;
    out += "\n\n";
  }
  return {out, idx - 1};
}

[[nodiscard]] std::optional<fs::path> find_worker_bin(const std::optional<fs::path>& override) {
  if (override.has_value() && fs::exists(*override)) {
    return *override;
  }
  if (const char* env = std::getenv("SUBLIFT_WORKER_PATH"); env != nullptr && *env != '\0') {
    fs::path p{env};
    if (fs::exists(p)) return p;
  }
  // Sibling of this executable: build/cpp/bin/sublift_worker next to sublift_cli / sublift
  std::error_code ec;
  fs::path self = fs::read_symlink("/proc/self/exe", ec);
#if defined(__APPLE__)
  // macOS: use argv0 resolution via _NSGetExecutablePath would be ideal; fall back
  // to common build locations relative to cwd.
  (void)self;
#endif
  // Prefer Release (cpp-rel) over Debug (cpp) for product performance.
  const std::vector<fs::path> candidates = {
      fs::current_path() / "build" / "cpp-rel" / "bin" / "sublift_worker",
      fs::current_path() / "build" / "cpp" / "bin" / "sublift_worker",
      fs::current_path() / "bin" / "sublift_worker",
      fs::path{"build/cpp-rel/bin/sublift_worker"},
      fs::path{"build/cpp/bin/sublift_worker"},
  };
  for (const auto& c : candidates) {
    if (fs::exists(c)) return fs::absolute(c);
  }
  return std::nullopt;
}

#if defined(__APPLE__)
#include <mach-o/dyld.h>
[[nodiscard]] std::optional<fs::path> executable_dir() {
  char buf[4096];
  uint32_t size = sizeof(buf);
  if (_NSGetExecutablePath(buf, &size) != 0) return std::nullopt;
  std::error_code ec;
  fs::path p = fs::weakly_canonical(fs::path{buf}, ec);
  if (ec) p = fs::path{buf};
  return p.parent_path();
}
#else
[[nodiscard]] std::optional<fs::path> executable_dir() {
  std::error_code ec;
  fs::path self = fs::read_symlink("/proc/self/exe", ec);
  if (ec) return std::nullopt;
  return self.parent_path();
}
#endif

[[nodiscard]] std::optional<fs::path> resolve_worker(const std::optional<fs::path>& override) {
  if (override.has_value() && fs::exists(*override)) return *override;
  if (const char* env = std::getenv("SUBLIFT_WORKER_PATH"); env != nullptr && *env != '\0') {
    fs::path p{env};
    if (fs::exists(p)) return p;
  }
  if (auto dir = executable_dir()) {
    fs::path sibling = *dir / "sublift_worker";
    if (fs::exists(sibling)) return sibling;
  }
  return find_worker_bin(std::nullopt);
}

struct ExtractArgs {
  fs::path video;
  fs::path output{"output.srt"};
  double fps{5.0};
  double confidence{0.5};
  std::string engine{"vision"};
  std::string script{"auto"};
  std::optional<fs::path> worker;
};

[[nodiscard]] int parse_extract_args(int argc, char** argv, ExtractArgs& out) {
  // argv[0]=prog, argv[1]=extract, rest options + video
  std::optional<std::string> positional;
  for (int i = 2; i < argc; ++i) {
    std::string_view arg = argv[i];
    auto need = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "Error: %s requires a value\n", name);
        return nullptr;
      }
      return argv[++i];
    };
    if (arg == "-o" || arg == "--output") {
      const char* v = need("--output");
      if (!v) return 2;
      out.output = v;
    } else if (arg == "--fps") {
      const char* v = need("--fps");
      if (!v) return 2;
      out.fps = std::atof(v);
    } else if (arg == "--confidence") {
      const char* v = need("--confidence");
      if (!v) return 2;
      out.confidence = std::atof(v);
    } else if (arg == "--engine") {
      const char* v = need("--engine");
      if (!v) return 2;
      out.engine = v;
    } else if (arg == "--script") {
      const char* v = need("--script");
      if (!v) return 2;
      out.script = v;
    } else if (arg == "--worker") {
      const char* v = need("--worker");
      if (!v) return 2;
      out.worker = fs::path{v};
    } else if (arg == "-h" || arg == "--help") {
      return -1;  // signal help
    } else if (!arg.empty() && arg[0] == '-') {
      std::fprintf(stderr, "Error: unknown option %s\n", argv[i]);
      return 2;
    } else {
      if (positional.has_value()) {
        std::fprintf(stderr, "Error: unexpected argument %s\n", argv[i]);
        return 2;
      }
      positional = std::string{arg};
    }
  }
  if (!positional.has_value()) {
    std::fprintf(stderr, "Error: extract requires <video> path\n");
    return 2;
  }
  out.video = *positional;
  if (out.engine != "mock" && out.engine != "vision" && out.engine != "paddle") {
    std::fprintf(stderr, "Error: --engine must be mock|vision|paddle\n");
    return 2;
  }
  if (out.script != "auto" && out.script != "cjk" && out.script != "latin") {
    std::fprintf(stderr, "Error: --script must be auto|cjk|latin\n");
    return 2;
  }
  return 0;
}

[[nodiscard]] bool wait_for_socket(const fs::path& sock, int timeout_ms = 5000) {
  const auto deadline =
      std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (std::chrono::steady_clock::now() < deadline) {
    if (fs::exists(sock)) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return false;
}

[[nodiscard]] int run_extract(const ExtractArgs& args) {
  if (!fs::exists(args.video)) {
    std::fprintf(stderr, "Error: video not found: %s\n", args.video.c_str());
    return 1;
  }

  auto worker = resolve_worker(args.worker);
  if (!worker.has_value()) {
    std::fprintf(stderr,
                 "Error: sublift_worker not found. Build C++ core "
                 "(cmake -S cpp -B build/cpp && cmake --build build/cpp) "
                 "or set SUBLIFT_WORKER_PATH / --worker.\n");
    return 1;
  }

  const fs::path sock =
      fs::temp_directory_path() /
      ("sublift_cli_" + std::to_string(static_cast<long long>(::getpid())) + ".sock");
  ::unlink(sock.c_str());

  const pid_t child = ::fork();
  if (child < 0) {
    std::perror("fork");
    return 1;
  }
  if (child == 0) {
    // Child: exec worker
    const std::string sock_s = sock.string();
    const std::string engine_s = args.engine;
    execl(worker->c_str(), worker->c_str(), "--socket", sock_s.c_str(), "--engine",
          engine_s.c_str(), static_cast<char*>(nullptr));
    std::perror("execl sublift_worker");
    _exit(127);
  }

  int client_fd = -1;
  int exit_code = 1;
  try {
    if (!wait_for_socket(sock)) {
      std::fprintf(stderr, "Error: worker socket did not appear: %s\n", sock.c_str());
      throw std::runtime_error("socket timeout");
    }

    client_fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (client_fd < 0) {
      std::perror("socket");
      throw std::runtime_error("socket");
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, sock.c_str(), sizeof(addr.sun_path) - 1);

    // Retry connect briefly
    bool connected = false;
    for (int i = 0; i < 50; ++i) {
      if (::connect(client_fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0) {
        connected = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    if (!connected) {
      std::perror("connect");
      throw std::runtime_error("connect");
    }

    // hello
    Message hello = HelloMsg{.client = "sublift_cli", .protocol_version = 1};
    auto hello_payload = serialize_message(hello);
    if (write_framed_message(client_fd, hello_payload) != FramingStatus::Success) {
      throw std::runtime_error("failed to send hello");
    }
    auto bye_frame = read_framed_message(client_fd);
    if (bye_frame.status != FramingStatus::Success) {
      throw std::runtime_error("failed to read bye");
    }
    auto bye_msg = parse_message(bye_frame.payload);
    if (!std::holds_alternative<ByeMsg>(bye_msg)) {
      throw std::runtime_error("handshake expected bye");
    }
    const auto& bye = std::get<ByeMsg>(bye_msg);
    std::string engines_summary;
    for (size_t i = 0; i < bye.engines.size(); ++i) {
      if (i > 0) engines_summary += ", ";
      engines_summary += bye.engines[i];
    }
    std::fprintf(stdout, "提取字幕 (native C++ Worker, engines: [%s])：%s\n",
                 engines_summary.c_str(), args.video.c_str());
    std::fprintf(stdout, "采样率：%.1ffps  引擎：%s  置信度：%.2f  文字系统：%s\n", args.fps,
                 args.engine.c_str(), args.confidence, args.script.c_str());

    StartJobMsg start_body{
        .video_id = "cli_job",
        .fps = args.fps,
        .engine = args.engine,
        .confidence_threshold = args.confidence,
        .video_path = fs::absolute(args.video).string(),
        .subtitle_profile =
            sublift::SubtitleProfile{
                .script = args.script,
                .center_x = 0,
                .center_y = 0,
                .height = 0,
                .y_min = 0,
                .y_max = 0,
            },
    };
    Message start = start_body;
    auto start_payload = serialize_message(start);
    if (write_framed_message(client_fd, start_payload) != FramingStatus::Success) {
      throw std::runtime_error("failed to send start_job");
    }

    std::vector<sublift::SubtitleEntry> entries;
    bool done_ok = false;
    std::string err;

    while (true) {
      auto frame = read_framed_message(client_fd);
      if (frame.status != FramingStatus::Success) {
        throw std::runtime_error("connection closed before job completion");
      }
      Message msg = parse_message(frame.payload);
      if (std::holds_alternative<ProgressMsg>(msg)) {
        const auto& p = std::get<ProgressMsg>(msg);
        if (::isatty(STDOUT_FILENO)) {
          std::fprintf(stdout, "\r[progress] %.1f%% (%s)", p.pct * 100.0, p.stage.c_str());
          std::fflush(stdout);
        }
      } else if (std::holds_alternative<EntriesMsg>(msg)) {
        entries = std::get<EntriesMsg>(msg).entries;
      } else if (std::holds_alternative<DoneMsg>(msg)) {
        const auto& d = std::get<DoneMsg>(msg);
        done_ok = d.ok;
        if (!d.ok) err = d.error.value_or("unknown");
        break;
      } else if (std::holds_alternative<ErrorMsg>(msg)) {
        err = std::get<ErrorMsg>(msg).message;
        break;
      }
    }

    if (::isatty(STDOUT_FILENO)) {
      std::fputc('\n', stdout);
    }

    if (!done_ok && !err.empty()) {
      std::fprintf(stderr, "Error: worker failed: %s\n", err.c_str());
      throw std::runtime_error(err);
    }

    // Ensure parent dirs
    if (args.output.has_parent_path()) {
      fs::create_directories(args.output.parent_path());
    }
    const auto [srt_body, written_count] = format_srt(entries);
    {
      std::ofstream ofs(args.output, std::ios::binary);
      if (!ofs) {
        std::fprintf(stderr, "Error: cannot write %s\n", args.output.c_str());
        throw std::runtime_error("write srt");
      }
      ofs << srt_body;
    }
    std::fprintf(stdout, "完成：%zu 条字幕 → %s\n", written_count, args.output.c_str());
    exit_code = 0;
  } catch (const std::exception& ex) {
    std::fprintf(stderr, "Error: %s\n", ex.what());
    exit_code = 1;
  }

  if (client_fd >= 0) {
    ::close(client_fd);
  }
  ::kill(child, SIGTERM);
  int status = 0;
  ::waitpid(child, &status, 0);
  ::unlink(sock.c_str());
  return exit_code;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    print_version();
    return 0;
  }

  std::string_view cmd = argv[1];
  if (cmd == "--version" || cmd == "-v" || cmd == "version") {
    print_version();
    return 0;
  }
  if (cmd == "-h" || cmd == "--help" || cmd == "help") {
    print_usage(argv[0]);
    return 0;
  }
  if (cmd == "extract") {
    ExtractArgs args;
    const int pr = parse_extract_args(argc, argv, args);
    if (pr == -1) {
      print_usage(argv[0]);
      return 0;
    }
    if (pr != 0) {
      print_usage(argv[0]);
      return pr;
    }
    return run_extract(args);
  }

  std::fprintf(stderr, "Error: unknown command '%s'\n", argv[1]);
  print_usage(argv[0]);
  return 2;
}
