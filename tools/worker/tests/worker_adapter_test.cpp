#include <catch2/catch_test_macros.hpp>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <nlohmann/json.hpp>
#include <opencv2/imgcodecs.hpp>
#include <stdexcept>
#include <string>
#include <vector>

#include <sublift/adapters/ffmpeg/ffmpeg_frame_source.hpp>

#if !defined(_WIN32)
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#ifndef SUBLIFT_WORKER_EXECUTABLE
#define SUBLIFT_WORKER_EXECUTABLE ""
#endif

namespace {

using nlohmann::json;

struct TempDirGuard {
  std::filesystem::path path;
  explicit TempDirGuard(const std::string& name) {
    path = std::filesystem::temp_directory_path() /
           ("sublift_worker_adapter_" + name + "_" +
            std::to_string(reinterpret_cast<std::uintptr_t>(this)));
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    std::filesystem::create_directories(path, ec);
  }
  ~TempDirGuard() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};

#if !defined(_WIN32)

bool generate_synthetic_mp4(const std::filesystem::path& video_path) {
  const std::string ffmpeg_bin = sublift::adapters::ffmpeg::resolve_ffmpeg_bin();
  const std::string cmd =
      "\"" + ffmpeg_bin + "\" -y -hide_banner -loglevel error "
      "-f lavfi -i \"color=c=black:s=640x360:d=2.5:r=10,"
      "drawbox=x=120:y=280:w=400:h=40:color=white@1.0:t=fill:enable='between(t,0.4,1.8)'\" "
      "-pix_fmt yuv420p -c:v libx264 \"" + video_path.string() + "\"";
  return std::system(cmd.c_str()) == 0;
}

class WorkerSubprocess {
 public:
  explicit WorkerSubprocess(const std::string& exe_path) {
    if (::pipe(in_pipe_) != 0 || ::pipe(out_pipe_) != 0) {
      throw std::runtime_error("pipe() failed");
    }
    pid_ = ::fork();
    if (pid_ < 0) {
      throw std::runtime_error("fork() failed");
    }
    if (pid_ == 0) {
      ::dup2(in_pipe_[0], STDIN_FILENO);
      ::dup2(out_pipe_[1], STDOUT_FILENO);
      ::close(in_pipe_[0]);
      ::close(in_pipe_[1]);
      ::close(out_pipe_[0]);
      ::close(out_pipe_[1]);
      char* const argv[] = {const_cast<char*>(exe_path.c_str()), nullptr};
      ::execv(exe_path.c_str(), argv);
      ::_exit(127);
    }
    ::close(in_pipe_[0]);
    in_pipe_[0] = -1;
    ::close(out_pipe_[1]);
    out_pipe_[1] = -1;
    out_fp_ = ::fdopen(out_pipe_[0], "r");
    if (!out_fp_) {
      throw std::runtime_error("fdopen() failed");
    }
  }

  ~WorkerSubprocess() {
    close_stdin();
    if (out_fp_) {
      ::fclose(out_fp_);
      out_fp_ = nullptr;
      out_pipe_[0] = -1;
    }
    if (pid_ > 0) {
      int status = 0;
      ::waitpid(pid_, &status, 0);
      pid_ = -1;
    }
  }

  void send_json(const json& req) {
    const std::string line = req.dump() + "\n";
    std::size_t off = 0;
    while (off < line.size()) {
      const ssize_t n = ::write(in_pipe_[1], line.data() + off, line.size() - off);
      if (n <= 0) {
        throw std::runtime_error("write() to worker stdin failed");
      }
      off += static_cast<std::size_t>(n);
    }
  }

  bool read_json(json& out) {
    if (!out_fp_) {
      return false;
    }
    std::string line;
    char buf[4096];
    while (std::fgets(buf, sizeof(buf), out_fp_) != nullptr) {
      line.append(buf);
      if (!line.empty() && line.back() == '\n') {
        line.pop_back();
        if (!line.empty() && line.back() == '\r') {
          line.pop_back();
        }
        out = json::parse(line);
        return true;
      }
    }
    return false;
  }

  void close_stdin() {
    if (in_pipe_[1] >= 0) {
      ::close(in_pipe_[1]);
      in_pipe_[1] = -1;
    }
  }

  int wait_exit() {
    close_stdin();
    if (pid_ <= 0) {
      return -1;
    }
    int status = 0;
    if (::waitpid(pid_, &status, 0) < 0) {
      return -1;
    }
    pid_ = -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
  }

 private:
  int in_pipe_[2]{-1, -1};
  int out_pipe_[2]{-1, -1};
  FILE* out_fp_{nullptr};
  pid_t pid_{-1};
};

#endif

}  // namespace

TEST_CASE("sublift_worker subprocess end-to-end stdio JSON-Lines adapter test",
          "[worker][adapter]") {
#if defined(_WIN32)
  SKIP("POSIX pipe test skipped on Windows");
#else
  if (!sublift::adapters::ffmpeg::available()) {
    SKIP("ffmpeg/ffprobe unavailable");
  }
  const std::string worker_exe = SUBLIFT_WORKER_EXECUTABLE;
  if (worker_exe.empty() || !std::filesystem::exists(worker_exe)) {
    SKIP("sublift_worker executable missing");
  }

  TempDirGuard tmp("e2e");
  const auto video_path = tmp.path / "synthetic.mp4";
  REQUIRE(generate_synthetic_mp4(video_path));

  WorkerSubprocess worker(worker_exe);

  // 1. capabilities
  worker.send_json({{"id", 1}, {"op", "capabilities"}, {"params", json::object()}});
  json msg;
  REQUIRE(worker.read_json(msg));
  REQUIRE(msg["id"] == 1);
  REQUIRE(msg["ok"] == true);
  REQUIRE(msg["data"]["ffmpeg_available"] == true);
  REQUIRE(msg["data"]["ffprobe_available"] == true);

  // 2. probe_video
  worker.send_json({{"id", 2}, {"op", "probe_video"}, {"params", {{"video_path", video_path.string()}}}});
  REQUIRE(worker.read_json(msg));
  REQUIRE(msg["id"] == 2);
  REQUIRE(msg["ok"] == true);
  REQUIRE(msg["data"]["width"] == 640);
  REQUIRE(msg["data"]["height"] == 360);
  REQUIRE(msg["data"]["duration_ms"].get<std::int64_t>() >= 2000);

  // 3. extract_frame (.png + max_width downscale, verified via OpenCV imread)
  const auto png_out = tmp.path / "frame_320.png";
  worker.send_json({{"id", 3},
                    {"op", "extract_frame"},
                    {"params",
                     {{"video_path", video_path.string()},
                      {"timestamp_ms", 1000},
                      {"max_width", 320},
                      {"out_path", png_out.string()}}}});
  REQUIRE(worker.read_json(msg));
  REQUIRE(msg["id"] == 3);
  REQUIRE(msg["ok"] == true);
  REQUIRE(msg["data"]["width"] == 320);
  REQUIRE(msg["data"]["height"] == 180);
  REQUIRE(std::filesystem::exists(png_out));
  cv::Mat decoded = cv::imread(png_out.string(), cv::IMREAD_COLOR);
  REQUIRE_FALSE(decoded.empty());
  REQUIRE(decoded.cols == 320);
  REQUIRE(decoded.rows == 180);

  // 4. run_extraction in pure_timing_strips mode
  const auto strips_dir = tmp.path / "strips";
  worker.send_json({{"id", 4},
                    {"op", "run_extraction"},
                    {"params",
                     {{"video_path", video_path.string()},
                      {"mode", "pure_timing_strips"},
                      {"strips_dir", strips_dir.string()},
                      {"region_box", {{"x", 0}, {"y", 250}, {"width", 640}, {"height", 110}}},
                      {"config", {{"sample_fps", 5.0}, {"subtitle_script", "auto"}}}}}});

  bool saw_timing_slot = false;
  bool saw_result = false;
  while (worker.read_json(msg)) {
    if (msg["id"] == 4 && msg["type"] == "event" && msg["event"] == "timing_slot") {
      saw_timing_slot = true;
      REQUIRE(std::filesystem::exists(msg["data"]["strip_path"].get<std::string>()));
    }
    if (msg["id"] == 4 && msg["type"] == "result") {
      REQUIRE(msg["ok"] == true);
      saw_result = true;
      break;
    }
  }
  REQUIRE(saw_timing_slot);
  REQUIRE(saw_result);

  // 5. finalize_srt
  worker.send_json({{"id", 5},
                    {"op", "finalize_srt"},
                    {"params",
                     {{"apply_merge", false},
                      {"entries",
                       json::array({{{"id", 1},
                                     {"start_ms", 400},
                                     {"end_ms", 1800},
                                     {"text", "Synthetic cue"},
                                     {"confidence", 0.99}}})}}}});
  REQUIRE(worker.read_json(msg));
  REQUIRE(msg["id"] == 5);
  REQUIRE(msg["ok"] == true);
  REQUIRE(msg["data"]["cue_count"] == 1);
  REQUIRE(msg["data"]["srt_text"].get<std::string>().find("Synthetic cue") != std::string::npos);

  // 6. Close stdin -> clean exit 0
  REQUIRE(worker.wait_exit() == 0);
#endif
}
