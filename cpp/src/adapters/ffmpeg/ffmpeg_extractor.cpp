#include "sublift/ffmpeg.hpp"

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <cmath>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>

#include "process_utils.hpp"

extern "C" char** environ;

namespace sublift::ffmpeg {

namespace {

struct TempLogFile {
  std::filesystem::path path;
  int fd{-1};

  TempLogFile() {
    auto temp_dir = std::filesystem::temp_directory_path();
    auto tmpl = (temp_dir / "sublift-ffmpeg-stderr-XXXXXX").string();
    std::vector<char> tmpl_buf(tmpl.begin(), tmpl.end());
    tmpl_buf.push_back('\0');
    fd = mkstemp(tmpl_buf.data());
    if (fd >= 0) {
      path = tmpl_buf.data();
    }
  }

  ~TempLogFile() {
    if (fd >= 0) {
      close(fd);
    }
    if (!path.empty()) {
      std::error_code ec;
      std::filesystem::remove(path, ec);
    }
  }

  TempLogFile(const TempLogFile&) = delete;
  TempLogFile& operator=(const TempLogFile&) = delete;
};

struct ScopedFd {
  int fd{-1};
  ScopedFd() = default;
  explicit ScopedFd(int f) : fd(f) {}
  ~ScopedFd() {
    if (fd >= 0) {
      ::close(fd);
    }
  }

  ScopedFd(const ScopedFd&) = delete;
  ScopedFd& operator=(const ScopedFd&) = delete;
};

struct ChildProcessGuard {
  pid_t pid{-1};
  std::mutex& mutex;
  pid_t& active_pid;
  bool reaped{false};

  ChildProcessGuard(pid_t p, std::mutex& m, pid_t& ap)
      : pid(p), mutex(m), active_pid(ap) {}

  ~ChildProcessGuard() {
    cancel_and_reap();
  }

  ChildProcessGuard(const ChildProcessGuard&) = delete;
  ChildProcessGuard& operator=(const ChildProcessGuard&) = delete;

  void cancel_and_reap() {
    std::lock_guard<std::mutex> lock(mutex);
    if (pid > 0 && !reaped) {
      ::kill(pid, SIGTERM);
      ::kill(pid, SIGKILL);
      int status = 0;
      ::waitpid(pid, &status, 0);
      reaped = true;
      active_pid = -1;
    }
  }

  int wait() {
    std::lock_guard<std::mutex> lock(mutex);
    if (pid > 0 && !reaped) {
      int status = 0;
      ::waitpid(pid, &status, 0);
      reaped = true;
      active_pid = -1;
      return status;
    }
    return 0;
  }
};

bool read_exact_bytes(int fd, uint8_t* dest, std::size_t count, const std::atomic<bool>& cancelled) {
  std::size_t read_bytes = 0;
  while (read_bytes < count) {
    if (cancelled.load(std::memory_order_acquire)) {
      return false;
    }
    ssize_t res = ::read(fd, dest + read_bytes, count - read_bytes);
    if (res > 0) {
      read_bytes += static_cast<std::size_t>(res);
    } else if (res == 0) {
      return false;  // EOF
    } else {
      if (errno == EINTR) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) {
        usleep(1000);
        continue;
      }
      return false;
    }
  }
  return true;
}

}  // namespace

FfmpegExtractor::FfmpegExtractor(double fps,
                                 std::optional<SourceBox> output_crop,
                                 std::optional<SourceFrameInfo> source_info)
    : fps_{fps}, output_crop_{output_crop}, source_info_{source_info} {
  if (std::isnan(fps) || fps <= 0.0) {
    throw std::invalid_argument("fps 必须为正数");
  }
}

FfmpegExtractor::~FfmpegExtractor() {
  cancel();
}

void FfmpegExtractor::cancel() {
  cancelled_.store(true, std::memory_order_release);
  std::lock_guard<std::mutex> lock(proc_mutex_);
  if (active_pid_ > 0) {
    ::kill(active_pid_, SIGTERM);
    ::kill(active_pid_, SIGKILL);
  }
}

void FfmpegExtractor::extract(const std::filesystem::path& video_path,
                             const std::function<bool(Frame)>& consumer) {
  if (cancelled_.load(std::memory_order_acquire)) {
    return;
  }

  std::error_code ec;
  if (!std::filesystem::exists(video_path, ec)) {
    throw std::runtime_error("视频文件不存在: " + video_path.string());
  }

  const std::string ffmpeg_bin = resolve_ffmpeg_bin();
  SourceFrameInfo source = source_info_.has_value() ? *source_info_ : probe_source_frame(video_path);

  if (output_crop_.has_value()) {
    validate_output_crop(*output_crop_, source.width, source.height);
  }

  const std::int32_t out_w = output_crop_.has_value() ? output_crop_->width : source.width;
  const std::int32_t out_h = output_crop_.has_value() ? output_crop_->height : source.height;
  const std::string vf = build_output_vf(fps_, output_crop_);
  const std::size_t frame_bytes = static_cast<std::size_t>(out_w) * out_h * 3;

  if (cancelled_.load(std::memory_order_acquire)) {
    return;
  }

  const std::vector<std::string> cmd = {
      ffmpeg_bin,
      "-nostdin",
      "-v",
      "error",
      "-i",
      video_path.string(),
      "-vf",
      vf,
      "-f",
      "image2pipe",
      "-pix_fmt",
      "rgb24",
      "-vcodec",
      "rawvideo",
      "-",
  };

  TempLogFile err_log;
  if (err_log.fd < 0) {
    throw std::runtime_error("无法创建临时 stderr 日志文件");
  }

  int stdout_pipe[2] = {-1, -1};
  if (pipe(stdout_pipe) != 0) {
    throw std::runtime_error("无法创建 stdout 管道");
  }

  ScopedFd stdout_read_fd{stdout_pipe[0]};
  ScopedFd stdout_write_fd{stdout_pipe[1]};

  int devnull_fd = open("/dev/null", O_RDONLY);
  ScopedFd devnull_guard{devnull_fd};

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);

  if (devnull_fd >= 0) {
    posix_spawn_file_actions_adddup2(&actions, devnull_fd, STDIN_FILENO);
  }
  posix_spawn_file_actions_adddup2(&actions, stdout_pipe[1], STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, err_log.fd, STDERR_FILENO);
  posix_spawn_file_actions_addclose(&actions, stdout_pipe[0]);
  if (devnull_fd >= 0) {
    posix_spawn_file_actions_addclose(&actions, devnull_fd);
  }

  std::vector<char*> argv;
  argv.reserve(cmd.size() + 1);
  for (const auto& arg : cmd) {
    argv.push_back(const_cast<char*>(arg.c_str()));
  }
  argv.push_back(nullptr);

  pid_t pid = 0;
  int spawn_err = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);

  posix_spawn_file_actions_destroy(&actions);
  stdout_write_fd.fd = -1;  // Parent closes write end
  ::close(stdout_pipe[1]);

  if (spawn_err != 0) {
    throw std::runtime_error("ffmpeg 子进程启动失败 (errno: " + std::to_string(spawn_err) + ")");
  }

  {
    std::lock_guard<std::mutex> lock(proc_mutex_);
    active_pid_ = pid;
  }

  ChildProcessGuard proc_guard{pid, proc_mutex_, active_pid_};

  if (cancelled_.load(std::memory_order_acquire)) {
    proc_guard.cancel_and_reap();
    return;
  }

  int frame_index = 0;
  bool user_stopped = false;

  while (!cancelled_.load(std::memory_order_acquire)) {
    ImageBuffer frame_img(out_w, out_h, PixelFormat::RGB24);
    if (!read_exact_bytes(stdout_read_fd.fd, frame_img.data(), frame_bytes, cancelled_)) {
      break;  // EOF or cancelled
    }

    std::int64_t timestamp_ms = static_cast<std::int64_t>(frame_index / fps_ * 1000.0);
    Frame frame{timestamp_ms, std::move(frame_img)};

    if (consumer) {
      if (!consumer(std::move(frame))) {
        user_stopped = true;
        break;
      }
    }
    frame_index++;
  }

  if (user_stopped || cancelled_.load(std::memory_order_acquire)) {
    proc_guard.cancel_and_reap();
  } else {
    int status = proc_guard.wait();
    if (WIFEXITED(status) && WEXITSTATUS(status) != 0) {
      std::string stderr_content;
      if (!err_log.path.empty() && std::filesystem::exists(err_log.path)) {
        std::ifstream ifs(err_log.path, std::ios::binary);
        if (ifs) {
          std::stringstream ss;
          ss << ifs.rdbuf();
          stderr_content = ss.str();
        }
      }
      std::string tail = detail::read_stderr_tail(stderr_content, 2000);
      std::string detail_msg = tail.empty() ? "" : "：" + tail;
      throw std::runtime_error("ffmpeg 抽帧失败（退出码 " +
                               std::to_string(WEXITSTATUS(status)) + "），path=" +
                               video_path.string() + detail_msg);
    }
  }
}

}  // namespace sublift::ffmpeg
