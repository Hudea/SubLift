#include <sublift/adapters/ffmpeg/ffmpeg_frame_source.hpp>
#include <sublift/adapters/ffmpeg/ffmpeg_stage_timing.hpp>

#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "process_utils.hpp"

extern "C" char** environ;

namespace sublift::adapters::ffmpeg {

namespace {

struct TempLogFile {
  std::filesystem::path path;
  int fd{-1};

  TempLogFile() {
    std::filesystem::path temp_dir;
    if (const char* env_tmp = std::getenv("SUBLIFT_TMPDIR"); env_tmp && *env_tmp) {
      temp_dir = env_tmp;
    } else {
      temp_dir = std::filesystem::temp_directory_path();
    }
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
  std::vector<pid_t>& active_pids;
  bool reaped{false};

  ChildProcessGuard(pid_t p, std::mutex& m, std::vector<pid_t>& pids)
      : pid(p), mutex(m), active_pids(pids) {}

  ~ChildProcessGuard() {
    cancel_and_reap();
  }

  ChildProcessGuard(const ChildProcessGuard&) = delete;
  ChildProcessGuard& operator=(const ChildProcessGuard&) = delete;

  void erase_pid_locked() {
    active_pids.erase(
        std::remove(active_pids.begin(), active_pids.end(), pid),
        active_pids.end());
  }

  void cancel_and_reap() {
    std::lock_guard<std::mutex> lock(mutex);
    if (pid > 0 && !reaped) {
      ::kill(pid, SIGTERM);
      ::kill(pid, SIGKILL);
      int status = 0;
      ::waitpid(pid, &status, 0);
      reaped = true;
      erase_pid_locked();
    }
  }

  int wait() {
    std::lock_guard<std::mutex> lock(mutex);
    if (pid > 0 && !reaped) {
      int status = 0;
      ::waitpid(pid, &status, 0);
      reaped = true;
      erase_pid_locked();
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

std::vector<std::int64_t> parse_showinfo_timestamps(const std::filesystem::path& log_path) {
  std::vector<std::int64_t> timestamps;
  if (log_path.empty() || !std::filesystem::exists(log_path)) {
    return timestamps;
  }
  std::ifstream ifs(log_path, std::ios::binary);
  if (!ifs) {
    return timestamps;
  }
  std::string line;
  while (std::getline(ifs, line)) {
    const auto pos = line.find("pts_time:");
    if (pos == std::string::npos) {
      continue;
    }
    std::size_t start = pos + 9;
    while (start < line.size() && line[start] == ' ') {
      start++;
    }
    std::size_t end = start;
    while (end < line.size() && (std::isdigit(static_cast<unsigned char>(line[end])) || line[end] == '.')) {
      end++;
    }
    if (end > start) {
      try {
        const double sec = std::stod(line.substr(start, end - start));
        timestamps.push_back(static_cast<std::int64_t>(std::llround(sec * 1000.0)));
      } catch (...) {
      }
    }
  }
  return timestamps;
}

}  // namespace

FfmpegFrameSource::FfmpegFrameSource(double fps,
                                     std::optional<SourceBox> output_crop,
                                     std::optional<SourceFrameInfo> source_info,
                                     std::optional<std::string> hwaccel,
                                     FfmpegExtractorOptions options)
    : fps_{fps},
      output_crop_{output_crop},
      source_info_{source_info},
      hwaccel_{std::move(hwaccel)},
      options_{options} {
  if (std::isnan(fps) || fps <= 0.0) {
    throw std::invalid_argument("fps 必须为正数");
  }
}

FfmpegFrameSource::~FfmpegFrameSource() {
  cancel();
}

void FfmpegFrameSource::set_time_range(std::optional<double> start_sec,
                                       std::optional<double> duration_sec) {
  if (start_sec.has_value() && (std::isnan(*start_sec) || *start_sec < 0.0)) {
    throw std::invalid_argument("start_sec 不能为负数或 NaN");
  }
  if (duration_sec.has_value() && (std::isnan(*duration_sec) || *duration_sec <= 0.0)) {
    throw std::invalid_argument("duration_sec 必须为正数");
  }
  start_sec_ = start_sec;
  duration_sec_ = duration_sec;
}

void FfmpegFrameSource::cancel() {
  cancelled_.store(true, std::memory_order_release);
  std::lock_guard<std::mutex> lock(proc_mutex_);
  for (pid_t pid : active_pids_) {
    if (pid > 0) {
      ::kill(pid, SIGTERM);
      ::kill(pid, SIGKILL);
    }
  }
}

bool FfmpegFrameSource::is_cancelled() const noexcept {
  return cancelled_.load(std::memory_order_acquire);
}

StreamDecodeHints FfmpegFrameSource::ensure_probed_hints(
    const std::filesystem::path& video_path, double fallback_fps) {
  if (source_info_.has_value() &&
      (!probed_video_path_.has_value() || *probed_video_path_ == video_path)) {
    if (cached_stream_hints_.has_value() && probed_video_path_ == video_path) {
      return *cached_stream_hints_;
    }
    return StreamDecodeHints{
        .width = source_info_->width,
        .height = source_info_->height,
        .fps = fallback_fps,
    };
  }
  const auto combined = probe_source_and_duration(video_path);
  source_info_ = combined.source;
  probed_video_path_ = video_path;
  cached_video_info_ = VideoInfo{
      .width = combined.source.width,
      .height = combined.source.height,
      .duration_ms = combined.duration_ms,
      .fps = combined.fps,
      .display_transform_ok = combined.source.display_transform_ok,
      .transform_note = combined.source.transform_note,
  };
  cached_stream_hints_ = StreamDecodeHints{
      .width = combined.source.width,
      .height = combined.source.height,
      .fps = combined.fps,
      .codec_name = combined.codec_name,
      .pix_fmt = combined.pix_fmt,
  };
  return *cached_stream_hints_;
}

std::string FfmpegFrameSource::resolve_requested_hwaccel() const {
  if (hwaccel_.has_value()) {
    return *hwaccel_;
  }
  if (const char* env_hw = std::getenv("SUBLIFT_FFMPEG_HWACCEL"); env_hw && *env_hw) {
    return env_hw;
  }
  return "auto";
}

std::string FfmpegFrameSource::resolve_requested_skip_frame() const {
  if (!options_.skip_frame.empty()) {
    return options_.skip_frame;
  }
  if (const char* env_sf = std::getenv("SUBLIFT_FFMPEG_SKIP_FRAME"); env_sf && *env_sf) {
    return env_sf;
  }
  return "auto";
}

VideoInfo FfmpegFrameSource::probe(const std::filesystem::path& video_path) {
  if (probed_video_path_.has_value() && *probed_video_path_ == video_path && cached_video_info_.has_value()) {
    return *cached_video_info_;
  }
  static_cast<void>(ensure_probed_hints(video_path));
  return *cached_video_info_;
}

FrameIOPlan FfmpegFrameSource::plan(
    const VideoInfo& info,
    const std::optional<SourceBox>& requested_box) {
  SourceFrameInfo source;
  if (source_info_.has_value()) {
    source = *source_info_;
  } else {
    source.width = info.width;
    source.height = info.height;
    source.display_transform_ok = info.display_transform_ok;
    source.transform_note = info.transform_note;
    source.pixel_aspect_num = 1;
    source.pixel_aspect_den = 1;
    source.relative_video_start_ms = 0;
  }
  const auto& target_crop = requested_box.has_value() ? requested_box : output_crop_;
  return plan_frame_io_pure(target_crop, "auto", source);
}

void FfmpegFrameSource::extract(const std::filesystem::path& video_path,
                                const std::function<bool(Frame)>& consumer) {
  extract(video_path, fps_, output_crop_, consumer);
}

void FfmpegFrameSource::extract(
    const std::filesystem::path& video_path,
    double fps,
    const std::optional<SourceBox>& crop,
    std::function<bool(Frame)> consumer) {
  if (std::isnan(fps) || fps <= 0.0) {
    throw std::invalid_argument("fps 必须为正数");
  }

  if (cancelled_.load(std::memory_order_acquire)) {
    return;
  }

  std::error_code ec;
  if (!std::filesystem::exists(video_path, ec)) {
    throw std::runtime_error("视频文件不存在: " + video_path.string());
  }

  const std::string ffmpeg_bin = resolve_ffmpeg_bin();
  const StreamDecodeHints stream_hints = ensure_probed_hints(video_path, 0.0);
  const SourceFrameInfo source = *source_info_;
  if (!source.display_transform_ok) {
    throw std::runtime_error("无法安全处理视频显示变换：" +
                             source.transform_note.value_or("未知显示变换"));
  }

  const auto& effective_crop = crop.has_value() ? crop : output_crop_;
  if (effective_crop.has_value()) {
    validate_output_crop(*effective_crop, source.width, source.height);
  }

  if (cancelled_.load(std::memory_order_acquire)) {
    return;
  }

  const std::string effective_hwaccel =
      resolve_effective_hwaccel(resolve_requested_hwaccel(), stream_hints);
  const std::string effective_skip_frame =
      resolve_effective_skip_frame(resolve_requested_skip_frame(), stream_hints, fps);

  const std::int64_t base_offset_ms =
      (start_sec_.has_value() && *start_sec_ > 0.0)
          ? static_cast<std::int64_t>(std::llround(*start_sec_ * 1000.0))
          : 0;

  // The rawvideo pipe contains no timestamps. The fps filter's first output
  // starts at the video stream's relative offset, rounded to its sampling grid.
  const auto leading_ms =
      std::max<std::int64_t>(0, source.relative_video_start_ms - base_offset_ms);
  const auto leading_frames =
      static_cast<std::int64_t>(std::llround(leading_ms * fps / 1000.0));
  const auto first_frame_ms =
      base_offset_ms + static_cast<std::int64_t>(leading_frames / fps * 1000.0);

  auto run_attempt = [&](const std::string& hw_mode, const std::string& skip_mode,
                         bool allow_fallback) -> bool {
    const ScaledOutputVfPlan vf_plan = plan_scaled_output_vf(
        fps, effective_crop, source.width, source.height, options_, hw_mode, stream_hints.pix_fmt);
    const std::string& vf = vf_plan.vf;
    const std::int32_t out_w = vf_plan.out_width;
    const std::int32_t out_h = vf_plan.out_height;
    const std::size_t frame_bytes = static_cast<std::size_t>(out_w) * out_h * 3;

    std::vector<std::string> cmd = {
        ffmpeg_bin,
        "-nostdin",
        "-v",
        "error",
        "-filter_threads",
        "1",
    };
    if (!hw_mode.empty()) {
      cmd.push_back("-hwaccel");
      cmd.push_back(hw_mode);
    }
    if (!skip_mode.empty()) {
      cmd.push_back("-skip_frame");
      cmd.push_back(skip_mode);
    }
    if (start_sec_.has_value() && *start_sec_ > 0.0) {
      std::ostringstream ss_start;
      ss_start << std::fixed << std::setprecision(3) << *start_sec_;
      cmd.push_back("-ss");
      cmd.push_back(ss_start.str());
    }
    if (duration_sec_.has_value() && *duration_sec_ > 0.0) {
      std::ostringstream ss_dur;
      ss_dur << std::fixed << std::setprecision(3) << *duration_sec_;
      cmd.push_back("-t");
      cmd.push_back(ss_dur.str());
    }
    cmd.insert(cmd.end(), {
        "-i",
        video_path.string(),
        "-vf",
        vf,
        "-fps_mode",
        "passthrough",
        "-f",
        "image2pipe",
        "-pix_fmt",
        "rgb24",
        "-vcodec",
        "rawvideo",
        "-",
    });

    TempLogFile err_log;
    if (err_log.fd < 0) {
      throw std::runtime_error("无法创建临时 stderr 日志文件");
    }

    const bool timing = sublift::stage_timing_enabled();
    auto setup_start = std::chrono::steady_clock::now();

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
      active_pids_.push_back(pid);
    }

    ChildProcessGuard proc_guard{pid, proc_mutex_, active_pids_};

    if (cancelled_.load(std::memory_order_acquire)) {
      proc_guard.cancel_and_reap();
      if (timing) {
        stage_registry().add(
            "extract_wait",
            std::chrono::duration<double, std::milli>(
                std::chrono::steady_clock::now() - setup_start)
                .count());
      }
      return true;
    }

    if (timing) {
      stage_registry().add(
          "extract_wait",
          std::chrono::duration<double, std::milli>(
              std::chrono::steady_clock::now() - setup_start)
              .count());
    }

    int frame_index = 0;
    bool user_stopped = false;

    while (!cancelled_.load(std::memory_order_acquire)) {
      std::optional<Frame> frame;
      {
        StageTimer fetch_timer(stage_registry(), "frame_fetch");
        std::optional<ImageBuffer> frame_img;
        {
          StageTimer materialize_timer(stage_registry(), "frame_materialize");
          frame_img.emplace(ImageBuffer::allocate_uninitialized(
              out_w, out_h, PixelFormat::RGB24));
        }
        if (!read_exact_bytes(stdout_read_fd.fd, frame_img->data(), frame_bytes, cancelled_)) {
          break;  // EOF or cancelled
        }

        std::int64_t timestamp_ms =
            first_frame_ms + static_cast<std::int64_t>(frame_index / fps * 1000.0);
        if (duration_sec_.has_value() &&
            static_cast<double>(timestamp_ms) >= base_offset_ms + *duration_sec_ * 1000.0) {
          user_stopped = true;
          break;
        }
        frame.emplace(timestamp_ms, std::move(*frame_img));
      }

      if (consumer) {
        StageTimer call_timer(stage_registry(), "consume_call");
        if (!consumer(std::move(*frame))) {
          user_stopped = true;
          break;
        }
      }
      frame_index++;
    }

    {
      StageTimer teardown_timer(stage_registry(), "extract_wait");
      if (user_stopped || cancelled_.load(std::memory_order_acquire)) {
        proc_guard.cancel_and_reap();
      } else {
        int status = proc_guard.wait();
        const bool exited_nonzero = WIFEXITED(status) && WEXITSTATUS(status) != 0;
        const bool signaled = WIFSIGNALED(status);
        if (exited_nonzero || signaled) {
          if (allow_fallback && (!hw_mode.empty() || !skip_mode.empty()) && frame_index == 0) {
            return false;  // Retry without hardware acceleration or frame skipping
          }
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
          const int code_or_sig =
              exited_nonzero ? WEXITSTATUS(status) : -WTERMSIG(status);
          throw std::runtime_error("ffmpeg 抽帧失败（退出码 " +
                                   std::to_string(code_or_sig) + "），path=" +
                                   video_path.string() + detail_msg);
        }
      }
    }
    return true;
  };

  if (!effective_hwaccel.empty()) {
    if (run_attempt(effective_hwaccel, effective_skip_frame, true)) {
      return;
    }
  }
  if (!effective_skip_frame.empty()) {
    if (run_attempt("", effective_skip_frame, true)) {
      return;
    }
  }
  static_cast<void>(run_attempt("", "", false));
}

ImageBuffer FfmpegFrameSource::extract_frame(
    const std::filesystem::path& video_path,
    std::int64_t timestamp_ms,
    const std::optional<SourceBox>& crop) {
  std::error_code ec;
  if (!std::filesystem::exists(video_path, ec)) {
    throw std::runtime_error("视频文件不存在: " + video_path.string());
  }

  const SourceFrameInfo info =
      source_info_.has_value() ? *source_info_ : probe_source_frame(video_path);
  if (!info.display_transform_ok) {
    throw std::runtime_error("无法安全处理视频显示变换：" +
                             info.transform_note.value_or("未知显示变换"));
  }

  const auto& effective_crop = crop.has_value() ? crop : output_crop_;
  if (effective_crop.has_value()) {
    validate_output_crop(*effective_crop, info.width, info.height);
  }

  const std::int32_t target_w =
      effective_crop.has_value() ? effective_crop->width : info.width;
  const std::int32_t target_h =
      effective_crop.has_value() ? effective_crop->height : info.height;

  if (target_w <= 0 || target_h <= 0) {
    throw std::runtime_error("目标图像尺寸无效: " + video_path.string());
  }

  const std::string ffmpeg_bin = resolve_ffmpeg_bin();
  std::ostringstream ss_time;
  const double time_seconds = std::max(0.0, static_cast<double>(timestamp_ms) / 1000.0);
  ss_time << std::fixed << std::setprecision(3) << time_seconds;

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

  if (effective_crop.has_value()) {
    std::string vf = "crop=" + std::to_string(effective_crop->width) + ":" +
                     std::to_string(effective_crop->height) + ":" +
                     std::to_string(effective_crop->x) + ":" +
                     std::to_string(effective_crop->y) + ":exact=1";
    cmd.push_back("-vf");
    cmd.push_back(vf);
  }

  cmd.push_back("-f");
  cmd.push_back("rawvideo");
  cmd.push_back("-pix_fmt");
  cmd.push_back("rgb24");
  cmd.push_back("-");

  auto res = detail::run_subprocess(cmd, std::chrono::milliseconds(10000));
  const std::size_t expected_bytes =
      static_cast<std::size_t>(target_w) * static_cast<std::size_t>(target_h) * 3;

  if (res.exit_code != 0 || res.stdout_str.size() < expected_bytes) {
    std::string tail = detail::read_stderr_tail(res.stderr_str, 500);
    std::string detail_msg = tail.empty() ? "" : "：" + tail;
    throw std::runtime_error("ffmpeg RGB24 截帧失败（退出码 " +
                             std::to_string(res.exit_code) + "），path=" +
                             video_path.string() + detail_msg);
  }

  ImageBuffer buffer(target_w, target_h, PixelFormat::RGB24);
  std::memcpy(buffer.data(), res.stdout_str.data(), expected_bytes);
  return buffer;
}

std::vector<KeyframeThumbnail> FfmpegFrameSource::sample_keyframe_thumbnails(
    const std::filesystem::path& video_path,
    double bottom_ratio,
    std::size_t max_samples) {
  if (cancelled_.load(std::memory_order_acquire)) {
    return {};
  }

  std::error_code ec;
  if (!std::filesystem::exists(video_path, ec)) {
    throw std::runtime_error("视频文件不存在: " + video_path.string());
  }

  const StreamDecodeHints stream_hints = ensure_probed_hints(video_path);
  const SourceFrameInfo source = *source_info_;
  if (!source.display_transform_ok) {
    throw std::runtime_error("无法安全处理视频显示变换：" +
                             source.transform_note.value_or("未知显示变换"));
  }

  const std::string effective_hwaccel =
      resolve_effective_hwaccel(resolve_requested_hwaccel(), stream_hints);

  const double effective_ratio = std::clamp(bottom_ratio, 0.05, 1.0);
  const std::string ffmpeg_bin = resolve_ffmpeg_bin();
  constexpr std::size_t kThumbBytes = 320 * 80 * 1;
  constexpr double kSparseWindowMinDurationSec = 600.0;

  double total_duration_sec = 0.0;
  if (cached_video_info_.has_value() && cached_video_info_->duration_ms > 0) {
    total_duration_sec = static_cast<double>(cached_video_info_->duration_ms) / 1000.0;
  } else if (!duration_sec_.has_value()) {
    try {
      const auto probed_dur_ms = probe_duration_ms(video_path);
      if (probed_dur_ms > 0) {
        total_duration_sec = static_cast<double>(probed_dur_ms) / 1000.0;
      }
    } catch (...) {
      total_duration_sec = 0.0;
    }
  }

  const double scan_start_sec =
      (start_sec_.has_value() && *start_sec_ > 0.0) ? *start_sec_ : 0.0;
  double eff_duration_sec = 0.0;
  if (duration_sec_.has_value() && *duration_sec_ > 0.0) {
    eff_duration_sec =
        (total_duration_sec > 0.0)
            ? std::min(*duration_sec_, std::max(0.0, total_duration_sec - scan_start_sec))
            : *duration_sec_;
  } else if (total_duration_sec > 0.0) {
    eff_duration_sec = std::max(0.0, total_duration_sec - scan_start_sec);
  }

  auto run_thumb_attempt =
      [&](const std::string& hw_mode,
          std::optional<double> win_start_opt,
          std::optional<double> win_dur_opt,
          std::optional<std::size_t> max_frames_opt)
      -> std::vector<KeyframeThumbnail> {
    if (cancelled_.load(std::memory_order_acquire)) {
      return {};
    }
    const bool use_nv12 =
        !hw_mode.empty() && is_high_bit_depth_pix_fmt(stream_hints.pix_fmt);
    const std::string gray_tail =
        use_nv12 ? "format=nv12,format=gray,showinfo" : "format=gray,showinfo";

    std::string vf;
    if (effective_ratio < 0.999) {
      const std::string crop_vf =
          "crop=iw:trunc(ih*" + std::to_string(effective_ratio) + "/2)*2:0:ih-trunc(ih*" +
          std::to_string(effective_ratio) + "/2)*2";
      vf = crop_vf + ",scale=320:80," + gray_tail;
    } else {
      vf = "scale=320:80," + gray_tail;
    }

    std::vector<std::string> cmd = {
        ffmpeg_bin,
        "-nostdin",
        "-hide_banner",
        "-v",
        "info",
        "-skip_frame",
        "nokey",
        "-discard",
        "nokey",
    };

    if (!hw_mode.empty()) {
      cmd.push_back("-hwaccel");
      cmd.push_back(hw_mode);
    }

    std::int64_t ts_offset_ms = 0;
    if (win_start_opt.has_value() && *win_start_opt > 0.0) {
      std::ostringstream ss_start;
      ss_start << std::fixed << std::setprecision(3) << *win_start_opt;
      cmd.push_back("-ss");
      cmd.push_back(ss_start.str());
      ts_offset_ms = static_cast<std::int64_t>(std::llround(*win_start_opt * 1000.0));
    }

    cmd.push_back("-i");
    cmd.push_back(video_path.string());

    if (win_dur_opt.has_value() && *win_dur_opt > 0.0) {
      std::ostringstream ss_dur;
      ss_dur << std::fixed << std::setprecision(3) << *win_dur_opt;
      cmd.push_back("-t");
      cmd.push_back(ss_dur.str());
    }
    if (max_frames_opt.has_value() && *max_frames_opt > 0) {
      cmd.push_back("-frames:v");
      cmd.push_back(std::to_string(*max_frames_opt));
    }

    cmd.insert(cmd.end(), {
        "-an",
        "-vf",
        vf,
        "-fps_mode",
        "passthrough",
        "-f",
        "rawvideo",
        "-pix_fmt",
        "gray",
        "-",
    });

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
    stdout_write_fd.fd = -1;
    ::close(stdout_pipe[1]);

    if (spawn_err != 0) {
      throw std::runtime_error("ffmpeg 缩略图提取子进程启动失败 (errno: " + std::to_string(spawn_err) + ")");
    }

    {
      std::lock_guard<std::mutex> lock(proc_mutex_);
      active_pids_.push_back(pid);
    }
    ChildProcessGuard proc_guard{pid, proc_mutex_, active_pids_};

    std::vector<ImageBuffer> attempt_images;
    while (!cancelled_.load(std::memory_order_acquire)) {
      ImageBuffer buf = ImageBuffer::allocate_uninitialized(320, 80, PixelFormat::Gray8);
      if (!read_exact_bytes(stdout_read_fd.fd, buf.data(), kThumbBytes, cancelled_)) {
        break;
      }
      attempt_images.push_back(std::move(buf));
    }

    proc_guard.wait();
    if (cancelled_.load(std::memory_order_acquire) || attempt_images.empty()) {
      return {};
    }

    auto timestamps = parse_showinfo_timestamps(err_log.path);
    std::vector<KeyframeThumbnail> thumbs;
    thumbs.reserve(attempt_images.size());
    for (std::size_t i = 0; i < attempt_images.size(); ++i) {
      const std::int64_t local_ts =
          (i < timestamps.size()) ? std::max<std::int64_t>(0, timestamps[i]) : 0;
      thumbs.push_back(KeyframeThumbnail{
          .timestamp_ms = ts_offset_ms + local_ts,
          .image = std::move(attempt_images[i]),
      });
    }
    return thumbs;
  };

  auto run_with_hw_fallback =
      [&](const std::string& preferred_hw,
          std::optional<double> win_start_opt,
          std::optional<double> win_dur_opt,
          std::optional<std::size_t> max_frames_opt)
      -> std::vector<KeyframeThumbnail> {
    if (!preferred_hw.empty()) {
      auto res = run_thumb_attempt(preferred_hw, win_start_opt, win_dur_opt, max_frames_opt);
      if (!res.empty() || cancelled_.load(std::memory_order_acquire)) {
        return res;
      }
    }
    if (!cancelled_.load(std::memory_order_acquire)) {
      return run_thumb_attempt("", win_start_opt, win_dur_opt, max_frames_opt);
    }
    return {};
  };

  std::vector<KeyframeThumbnail> thumbnails;
  if (eff_duration_sec > kSparseWindowMinDurationSec && max_samples > 0) {
    // 长视频采用稀疏短窗并发关键帧抽样：避免在 1s-GOP 全片上解码数千帧后丢弃 97%
    const std::size_t num_windows = std::clamp<std::size_t>(max_samples / 10, 16, 24);
    const double win_sec = std::clamp(
        eff_duration_sec / (static_cast<double>(num_windows) * 5.0), 12.0, 20.0);
    const double usable_start = scan_start_sec + eff_duration_sec * 0.02;
    const double usable_end = std::max(
        usable_start, scan_start_sec + eff_duration_sec * 0.98 - win_sec);
    const double stride =
        (num_windows > 1)
            ? (usable_end - usable_start) / static_cast<double>(num_windows - 1)
            : 0.0;
    const std::size_t per_win_cap = std::clamp<std::size_t>(
        ((max_samples + num_windows - 1) / num_windows) * 2, 4, 16);

    // 1080p 及以下 8-bit nokey 解码下软解显著快于 VideoToolbox 显存回传，保留硬件仅用于超高清/高色深
    const bool prefer_sw_nokey =
        (source.width <= 1920 && source.height <= 1080 &&
         !is_high_bit_depth_pix_fmt(stream_hints.pix_fmt));
    const std::string sparse_hw = prefer_sw_nokey ? std::string("") : effective_hwaccel;

    std::vector<std::vector<KeyframeThumbnail>> window_results(num_windows);
    std::atomic<std::size_t> next_win{0};
    std::mutex err_mu;
    std::exception_ptr worker_err;
    const std::size_t num_threads = std::min<std::size_t>(4, num_windows);

    {
      std::vector<std::thread> workers;
      workers.reserve(num_threads);
      for (std::size_t t = 0; t < num_threads; ++t) {
        workers.emplace_back([&]() {
          try {
            while (!cancelled_.load(std::memory_order_acquire)) {
              const std::size_t idx = next_win.fetch_add(1, std::memory_order_relaxed);
              if (idx >= num_windows) {
                break;
              }
              const double win_start = usable_start + static_cast<double>(idx) * stride;
              const double max_allowed =
                  std::max(1.0, scan_start_sec + eff_duration_sec - win_start);
              const double cur_win_dur = std::min(win_sec, max_allowed);
              window_results[idx] = run_with_hw_fallback(
                  sparse_hw, win_start, cur_win_dur, per_win_cap);
            }
          } catch (...) {
            std::lock_guard<std::mutex> lock(err_mu);
            if (!worker_err) {
              worker_err = std::current_exception();
            }
          }
        });
      }
      for (auto& w : workers) {
        if (w.joinable()) {
          w.join();
        }
      }
    }

    if (worker_err && !cancelled_.load(std::memory_order_acquire)) {
      std::rethrow_exception(worker_err);
    }
    if (cancelled_.load(std::memory_order_acquire)) {
      return {};
    }

    for (auto& bucket : window_results) {
      for (auto& item : bucket) {
        thumbnails.push_back(std::move(item));
      }
    }
    std::stable_sort(thumbnails.begin(), thumbnails.end(),
                     [](const KeyframeThumbnail& a, const KeyframeThumbnail& b) {
                       return a.timestamp_ms < b.timestamp_ms;
                     });
    // 去重相邻极近窗口重叠帧（<= 40ms）
    std::vector<KeyframeThumbnail> deduped;
    deduped.reserve(thumbnails.size());
    for (auto& item : thumbnails) {
      if (deduped.empty() || item.timestamp_ms - deduped.back().timestamp_ms > 40) {
        deduped.push_back(std::move(item));
      }
    }
    thumbnails = std::move(deduped);
  }

  if (thumbnails.empty() && !cancelled_.load(std::memory_order_acquire)) {
    thumbnails = run_with_hw_fallback(
        effective_hwaccel, start_sec_, duration_sec_, std::nullopt);
  }

  if (cancelled_.load(std::memory_order_acquire) || thumbnails.empty()) {
    return {};
  }

  // 若采样数量超出 max_samples，进行全时段等距均匀降采样
  if (max_samples > 0 && thumbnails.size() > max_samples) {
    std::vector<KeyframeThumbnail> downsampled;
    downsampled.reserve(max_samples);
    if (max_samples == 1) {
      downsampled.push_back(std::move(thumbnails.front()));
    } else {
      const double step = static_cast<double>(thumbnails.size() - 1) / (max_samples - 1);
      for (std::size_t i = 0; i < max_samples; ++i) {
        const std::size_t idx = static_cast<std::size_t>(std::llround(i * step));
        downsampled.push_back(std::move(thumbnails[idx]));
      }
    }
    return downsampled;
  }

  return thumbnails;
}

ImageBuffer extract_single_frame_rgb24(
    const std::filesystem::path& video_path,
    double time_seconds,
    int max_dimension) {
  std::error_code ec;
  if (!std::filesystem::exists(video_path, ec)) {
    throw std::runtime_error("视频文件不存在: " + video_path.string());
  }

  const SourceFrameInfo info = probe_source_frame(video_path);
  if (!info.display_transform_ok) {
    throw std::runtime_error("无法安全处理视频显示变换：" +
                             info.transform_note.value_or("未知显示变换"));
  }
  std::int32_t src_w = info.width;
  std::int32_t src_h = info.height;
  if (src_w <= 0 || src_h <= 0) {
    throw std::runtime_error("无法获取视频有效分辨率: " + video_path.string());
  }

  std::int32_t target_w = src_w;
  std::int32_t target_h = src_h;

  if (max_dimension > 0 && (src_w > max_dimension || src_h > max_dimension)) {
    if (src_w >= src_h) {
      target_w = max_dimension;
      target_h = static_cast<std::int32_t>(
          (static_cast<std::int64_t>(src_h) * max_dimension / src_w) / 2 * 2);
    } else {
      target_h = max_dimension;
      target_w = static_cast<std::int32_t>(
          (static_cast<std::int64_t>(src_w) * max_dimension / src_h) / 2 * 2);
    }
  } else {
    target_w = (src_w / 2) * 2;
    target_h = (src_h / 2) * 2;
  }
  if (target_w <= 0) target_w = 2;
  if (target_h <= 0) target_h = 2;

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
      "-vf",
      "scale=" + std::to_string(target_w) + ":" + std::to_string(target_h),
      "-f",
      "rawvideo",
      "-pix_fmt",
      "rgb24",
      "-",
  };

  auto res = detail::run_subprocess(cmd, std::chrono::milliseconds(5000));
  const std::size_t expected_bytes =
      static_cast<std::size_t>(target_w) * static_cast<std::size_t>(target_h) * 3;

  if (res.exit_code != 0 || res.stdout_str.size() < expected_bytes) {
    std::string tail = detail::read_stderr_tail(res.stderr_str, 500);
    std::string detail_msg = tail.empty() ? "" : "：" + tail;
    throw std::runtime_error("ffmpeg RGB24 内存截帧失败（退出码 " +
                             std::to_string(res.exit_code) + "），path=" +
                             video_path.string() + detail_msg);
  }

  ImageBuffer buffer(target_w, target_h, PixelFormat::RGB24);
  std::memcpy(buffer.data(), res.stdout_str.data(), expected_bytes);
  return buffer;
}

}  // namespace sublift::adapters::ffmpeg
