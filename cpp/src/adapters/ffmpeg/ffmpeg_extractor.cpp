#include "sublift/adapters/ffmpeg.hpp"

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
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>
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

std::vector<std::uint8_t> extract_single_frame_jpeg(
    const std::filesystem::path& video_path,
    double time_seconds,
    const std::optional<SourceBox>& crop,
    int quality) {
  std::error_code ec;
  if (!std::filesystem::exists(video_path, ec)) {
    throw std::runtime_error("视频文件不存在: " + video_path.string());
  }

  const std::string ffmpeg_bin = resolve_ffmpeg_bin();

  std::ostringstream ss_time;
  ss_time << std::fixed << std::setprecision(3) << std::max(0.0, time_seconds);

  std::vector<std::string> cmd = {
      ffmpeg_bin,
      "-ss",
      ss_time.str(),
      "-nostdin",
      "-v",
      "error",
      "-i",
      video_path.string(),
      "-frames:v",
      "1",
  };

  if (crop.has_value()) {
    std::string vf = "crop=" + std::to_string(crop->width) + ":" +
                     std::to_string(crop->height) + ":" +
                     std::to_string(crop->x) + ":" +
                     std::to_string(crop->y) + ":exact=1";
    cmd.push_back("-vf");
    cmd.push_back(vf);
  }

  cmd.push_back("-q:v");
  cmd.push_back(std::to_string(std::clamp(quality, 1, 31)));
  cmd.push_back("-f");
  cmd.push_back("image2");
  cmd.push_back("-c:v");
  cmd.push_back("mjpeg");
  cmd.push_back("-");

  auto res = detail::run_subprocess(cmd, std::chrono::milliseconds(10000));
  if (res.exit_code != 0 || res.stdout_str.empty()) {
    std::string tail = detail::read_stderr_tail(res.stderr_str, 500);
    std::string detail_msg = tail.empty() ? "" : "：" + tail;
    throw std::runtime_error("ffmpeg 截帧失败（退出码 " +
                             std::to_string(res.exit_code) + "），path=" +
                             video_path.string() + detail_msg);
  }

  return std::vector<std::uint8_t>(res.stdout_str.begin(), res.stdout_str.end());
}

std::filesystem::path remux_to_faststart_mp4(
    const std::filesystem::path& video_path,
    const std::optional<std::filesystem::path>& custom_cache_dir) {
  std::error_code ec;
  if (!std::filesystem::exists(video_path, ec) || !std::filesystem::is_regular_file(video_path, ec)) {
    throw std::runtime_error("Video file does not exist: " + video_path.string());
  }

  // Determine cache directory
  std::filesystem::path cache_dir;
  if (custom_cache_dir.has_value()) {
    cache_dir = *custom_cache_dir;
  } else if (const char* env_dir = std::getenv("SUBLIFT_PREVIEW_CACHE_DIR"); env_dir && *env_dir) {
    cache_dir = env_dir;
  } else {
    auto tmp = std::filesystem::temp_directory_path();
    cache_dir = tmp / "sublift_preview_cache";
  }

  std::filesystem::create_directories(cache_dir, ec);

  // Compute a deterministic cache key from path, size, and last write time
  auto fsize = std::filesystem::file_size(video_path, ec);
  auto ftime = std::filesystem::last_write_time(video_path, ec);
  auto time_val = static_cast<std::int64_t>(ftime.time_since_epoch().count());

  std::string key_str = video_path.string() + "_" + std::to_string(fsize) + "_" + std::to_string(time_val);
  std::size_t h = std::hash<std::string>{}(key_str);

  std::ostringstream ss_name;
  ss_name << "preview_" << std::hex << h << ".mp4";
  std::filesystem::path final_target = cache_dir / ss_name.str();

  // If cache exists and is non-empty, reuse it!
  if (std::filesystem::exists(final_target, ec) && std::filesystem::file_size(final_target, ec) > 0) {
    return final_target;
  }

  std::ostringstream ss_temp;
  ss_temp << "preview_" << std::hex << h << "_tmp_" << std::to_string(::getpid()) << "_"
          << std::this_thread::get_id() << ".mp4";
  std::filesystem::path temp_target = cache_dir / ss_temp.str();

  const std::string ffmpeg_bin = resolve_ffmpeg_bin();

  // 1. Try fast remux (-map 0:v:0 -map 0:a? -c:v copy -c:a aac -movflags +faststart -f mp4)
  std::vector<std::string> copy_cmd = {
      ffmpeg_bin,
      "-y",
      "-nostdin",
      "-v", "error",
      "-i", video_path.string(),
      "-map", "0:v:0",
      "-map", "0:a?",
      "-c:v", "copy",
      "-c:a", "aac",
      "-movflags", "+faststart",
      "-f", "mp4",
      temp_target.string()
  };

  auto res = detail::run_subprocess(copy_cmd, std::chrono::milliseconds(30000));
  bool success = (res.exit_code == 0 && std::filesystem::exists(temp_target, ec) && std::filesystem::file_size(temp_target, ec) > 0);

  // 2. If copy failed, fallback to ultrafast transcode (-pix_fmt yuv420p for 100% browser compatibility)
  if (!success) {
    std::filesystem::remove(temp_target, ec);
    std::vector<std::string> trans_cmd = {
        ffmpeg_bin,
        "-y",
        "-nostdin",
        "-v", "error",
        "-i", video_path.string(),
        "-map", "0:v:0",
        "-map", "0:a?",
        "-c:v", "libx264",
        "-preset", "ultrafast",
        "-crf", "26",
        "-pix_fmt", "yuv420p",
        "-c:a", "aac",
        "-movflags", "+faststart",
        "-f", "mp4",
        temp_target.string()
    };
    auto trans_res = detail::run_subprocess(trans_cmd, std::chrono::milliseconds(60000));
    if (trans_res.exit_code != 0 || !std::filesystem::exists(temp_target, ec) || std::filesystem::file_size(temp_target, ec) == 0) {
      std::filesystem::remove(temp_target, ec);
      std::string tail = detail::read_stderr_tail(trans_res.stderr_str, 500);
      throw std::runtime_error("ffmpeg 转封装/转码预览流失败（" + video_path.string() + "）：" + tail);
    }
  }

  std::filesystem::rename(temp_target, final_target, ec);
  if (ec) {
    std::filesystem::copy_file(temp_target, final_target, std::filesystem::copy_options::overwrite_existing, ec);
    std::filesystem::remove(temp_target, ec);
  }

  return final_target;
}

}  // namespace sublift::ffmpeg
