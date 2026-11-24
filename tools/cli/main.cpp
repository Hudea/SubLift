#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <io.h>
#define isatty_compat(fd) _isatty(fd)
#define fileno_compat(f) _fileno(f)
#else
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>
#define isatty_compat(fd) isatty(fd)
#define fileno_compat(f) fileno(f)
#endif

#include <sublift/adapters/ffmpeg/bottom_crop_detector.hpp>
#include <sublift/adapters/ffmpeg/ffmpeg_frame_source.hpp>
#include <sublift/adapters/ffmpeg/fixed_detector.hpp>
#include <sublift/adapters/paddle/paddle_ocr.hpp>
#include <sublift/core/box.hpp>
#include <sublift/core/subtitle.hpp>
#include <sublift/core/version.hpp>
#include <sublift/engine/auto_region.hpp>
#include <sublift/engine/config.hpp>
#include <sublift/engine/observer.hpp>
#include <sublift/engine/session.hpp>
#include <sublift/ports/ocr_engine.hpp>
#include <sublift/text/srt.hpp>

namespace {

class NullOcrEngine final : public sublift::IOcrEngine {
 public:
  sublift::OcrResult recognize(const sublift::ImageView&) override { return {}; }
  [[nodiscard]] std::string engine_name() const override { return "null"; }
};

constexpr std::string_view kVersion = sublift::version_string();

static volatile std::sig_atomic_t g_signal_received = 0;
#if !defined(_WIN32)
static int g_signal_pipe_wr = -1;
#endif

void handle_signal(int sig) {
  g_signal_received = sig;
#if !defined(_WIN32)
  if (g_signal_pipe_wr >= 0) {
    const unsigned char byte = static_cast<unsigned char>(sig);
    const auto written = ::write(g_signal_pipe_wr, &byte, 1);
    (void)written;
  }
#endif
}

class SignalWatcher {
 public:
  SignalWatcher() {
#if defined(_WIN32)
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);
    watcher_ = std::thread([this]() {
      while (!stopping_.load(std::memory_order_acquire)) {
        if (g_signal_received != 0) {
          on_signal_triggered();
          return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
      }
    });
#else
    if (::pipe(sig_pipe_) == 0 && ::pipe(stop_pipe_) == 0) {
      set_nonblock_cloexec(sig_pipe_[0]);
      set_nonblock_cloexec(sig_pipe_[1]);
      set_nonblock_cloexec(stop_pipe_[0]);
      set_nonblock_cloexec(stop_pipe_[1]);
      g_signal_pipe_wr = sig_pipe_[1];
      pipes_ready_ = true;
    }

    struct sigaction sa{};
    sa.sa_handler = handle_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    if (pipes_ready_) {
      watcher_ = std::thread([this]() {
        struct pollfd pfds[2]{};
        pfds[0].fd = sig_pipe_[0];
        pfds[0].events = POLLIN;
        pfds[1].fd = stop_pipe_[0];
        pfds[1].events = POLLIN;

        while (true) {
          const int rc = ::poll(pfds, 2, -1);
          if (rc < 0) {
            continue;
          }
          if ((pfds[0].revents & (POLLIN | POLLHUP)) != 0) {
            on_signal_triggered();
            return;
          }
          if ((pfds[1].revents & (POLLIN | POLLHUP)) != 0) {
            return;
          }
        }
      });
    }
#endif
  }

  ~SignalWatcher() {
    stopping_.store(true, std::memory_order_release);
#if !defined(_WIN32)
    g_signal_pipe_wr = -1;
    if (pipes_ready_) {
      const unsigned char stop_byte = 1;
      const auto written = ::write(stop_pipe_[1], &stop_byte, 1);
      (void)written;
    }
#endif
    if (watcher_.joinable()) {
      watcher_.join();
    }
#if !defined(_WIN32)
    if (pipes_ready_) {
      ::close(sig_pipe_[0]);
      ::close(sig_pipe_[1]);
      ::close(stop_pipe_[0]);
      ::close(stop_pipe_[1]);
    }
#endif
  }

  SignalWatcher(const SignalWatcher&) = delete;
  SignalWatcher& operator=(const SignalWatcher&) = delete;

  void bind_source(sublift::IFrameSource* source) {
    std::lock_guard<std::mutex> lock(mu_);
    source_ = source;
  }

  void bind_session(sublift::ExtractionSession* session) {
    std::lock_guard<std::mutex> lock(mu_);
    session_ = session;
  }

  void unbind_all() {
    std::lock_guard<std::mutex> lock(mu_);
    session_ = nullptr;
    source_ = nullptr;
    post_extract_ = true;
  }

 private:
#if !defined(_WIN32)
  static void set_nonblock_cloexec(int fd) noexcept {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags >= 0) {
      (void)::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    }
    const int fd_flags = ::fcntl(fd, F_GETFD, 0);
    if (fd_flags >= 0) {
      (void)::fcntl(fd, F_SETFD, fd_flags | FD_CLOEXEC);
    }
  }
#endif

  void on_signal_triggered() {
    std::lock_guard<std::mutex> lock(mu_);
    if (post_extract_) {
      return;
    }
    if (session_ != nullptr) {
      session_->cancel();
      return;
    }
    if (source_ != nullptr) {
      source_->cancel();
    }
    std::fprintf(stdout, "\n提取已被用户取消。\n");
    std::fflush(stdout);
    _exit(130);
  }

  std::mutex mu_;
  sublift::IFrameSource* source_{nullptr};
  sublift::ExtractionSession* session_{nullptr};
  bool post_extract_{false};
  std::atomic<bool> stopping_{false};
#if !defined(_WIN32)
  int sig_pipe_[2]{-1, -1};
  int stop_pipe_[2]{-1, -1};
  bool pipes_ready_{false};
#endif
  std::thread watcher_;
};

struct CliOptions {
  std::filesystem::path video_path;
  std::filesystem::path output_path{"output.srt"};
  double fps{5.0};
  double confidence{0.0};
  double bottom_ratio{0.3};
  std::size_t threads{0};
  std::string model_dir;
  std::string script{"auto"};
  std::optional<sublift::SourceBox> region_box;
  bool auto_region{false};
  double det_pixel_budget{sublift::adapters::paddle::kProductDetPixelBudget};
  int ort_threads{4};
  bool timing_only{false};
  std::string hwaccel{"auto"};
  std::string skip_frame{};
  std::optional<std::int64_t> start_ms{std::nullopt};
  std::optional<std::int64_t> end_ms{std::nullopt};
  std::int32_t max_analysis_width{-1};
  std::filesystem::path export_strips_dir;
  bool show_help{false};
  bool show_version{false};
};

[[nodiscard]] bool is_supported_hwaccel(std::string_view value, std::string& normalized) {
  normalized.clear();
  normalized.reserve(value.size());
  for (unsigned char ch : value) {
    normalized.push_back(static_cast<char>(std::tolower(ch)));
  }
  static constexpr std::array<std::string_view, 14> kAllowed = {
      "auto", "none", "off", "false", "0",
      "videotoolbox", "cuda", "vaapi", "qsv",
      "dxva2", "d3d11va", "vulkan", "opencl", "mediacodec",
  };
  return std::find(kAllowed.begin(), kAllowed.end(), normalized) != kAllowed.end();
}

[[nodiscard]] bool is_supported_skip_frame(std::string_view value, std::string& normalized) {
  std::string lower;
  lower.reserve(value.size());
  for (unsigned char ch : value) {
    lower.push_back(static_cast<char>(std::tolower(ch)));
  }
  if (lower == "auto") {
    normalized = "auto";
    return true;
  }
  if (lower == "none" || lower == "off" || lower == "false" || lower == "0") {
    normalized = "none";
    return true;
  }
  if (lower == "noref" || lower == "nonref") {
    normalized = "noref";
    return true;
  }
  return false;
}

[[nodiscard]] bool parse_strict_i64(const std::string& raw, std::int64_t& out) {
  if (raw.empty()) {
    return false;
  }
  try {
    std::size_t pos = 0;
    const long long val = std::stoll(raw, &pos, 10);
    if (pos != raw.size()) {
      return false;
    }
    out = static_cast<std::int64_t>(val);
    return true;
  } catch (...) {
    return false;
  }
}

void print_version() {
  std::fprintf(stdout, "sublift_cli version %.*s\n",
               static_cast<int>(kVersion.size()), kVersion.data());
}

void print_usage(const char* prog_name) {
  std::fprintf(stdout,
               "sublift_cli - 视频硬字幕提取与时间轴对齐命令行工具\n\n"
               "用法:\n"
               "  %s [选项] <视频路径>\n"
               "  %s extract <视频路径> [选项]\n"
               "  %s timing <视频路径> [选项]\n\n"
               "参数:\n"
               "  <视频路径>                输入视频文件路径 (也可通过 -i / --input 指定)\n\n"
               "选项:\n"
               "  -i, --input PATH          输入视频文件路径\n"
               "  -o, --output PATH         输出字幕文件路径，支持 .srt / .jsonl (默认: output.srt)\n"
               "  --fps FLOAT               采样帧率 (默认: 5.0)\n"
               "  --confidence FLOAT        OCR 最小置信度阈值 (默认: 0.0)\n"
               "  --bottom-ratio FLOAT      画面底部裁剪比例 [0.0 ~ 1.0] (默认: 0.3)\n"
               "  --region x,y,w,h          指定字幕裁剪区域坐标 (像素: x,y,w,h)\n"
               "  --auto-region             自动探测视频字幕区域（与 --region 互斥）\n"
               "  --timing-only             纯打轴模式：跳过 OCR，仅检测并输出字幕时间段 (别名: --pure-timing)\n"
               "  --threads INT             工作线程数 (默认: 0，自适应物理核心数)\n"
               "  --ort-threads INT         ONNX Runtime 推理并发线程数 (默认: 4)\n"
               "  --det-pixel-budget FLOAT  PaddleOCR 检测网络像素预算 (默认: 600000.0)\n"
               "  --hwaccel MODE            FFmpeg 硬件加速解码模式 (auto|none|videotoolbox|cuda|vaapi|qsv|d3d11va，默认: auto)\n"
               "  --skip-frame MODE         参考帧跳帧策略 (auto|none|noref，默认: auto，>=50fps 自动启用 noref)\n"
               "  --start-ms INT            起始截取时间戳 (毫秒，>= 0)\n"
               "  --end-ms INT              结束截取时间戳 (毫秒，须大于 --start-ms)\n"
               "  --max-analysis-width INT  分析阶段缩放宽度上限 (默认: 纯打轴 1920，完整提取 0 不缩放)\n"
               "  --export-strips-dir PATH  纯打轴单遍导出的中点字幕条带 PPM 输出目录 (须配合 --timing-only)\n"
               "  --model-dir PATH          自定义 PaddleOCR 模型根目录路径 (默认: 自动发现)\n"
               "  --script SCRIPT           文字语言系统 (auto|cjk|latin，默认: auto)\n"
               "  -v, --version             显示版本信息\n"
               "  -h, --help                显示帮助信息\n\n"
               "示例:\n"
               "  %s video.mp4 -o result.srt --fps 5.0 --bottom-ratio 0.25\n",
               prog_name, prog_name, prog_name, prog_name);
}

bool parse_args(int argc, char** argv, CliOptions& opts, std::string& err) {
  std::vector<std::string> args;
  args.reserve(static_cast<std::size_t>(argc));
  for (int i = 1; i < argc; ++i) {
    args.emplace_back(argv[i]);
  }

  if (args.empty()) {
    opts.show_help = true;
    return true;
  }

  std::size_t i = 0;
  if (i < args.size() && args[i] == "extract") {
    ++i;
  } else if (i < args.size() && args[i] == "timing") {
    opts.timing_only = true;
    ++i;
  }

  for (; i < args.size(); ++i) {
    const std::string& arg = args[i];

    if (arg == "-h" || arg == "--help" || arg == "help") {
      opts.show_help = true;
      return true;
    }
    if (arg == "-v" || arg == "--version" || arg == "version") {
      opts.show_version = true;
      return true;
    }

    if (arg == "-i" || arg == "--input") {
      if (i + 1 >= args.size()) {
        err = "选项 " + arg + " 缺少参数值";
        return false;
      }
      opts.video_path = args[++i];
    } else if (arg == "-o" || arg == "--output") {
      if (i + 1 >= args.size()) {
        err = "选项 " + arg + " 缺少参数值";
        return false;
      }
      opts.output_path = args[++i];
    } else if (arg == "--fps") {
      if (i + 1 >= args.size()) {
        err = "选项 " + arg + " 缺少参数值";
        return false;
      }
      try {
        opts.fps = std::stod(args[++i]);
        if (opts.fps <= 0.0) {
          err = "--fps 必须为正数";
          return false;
        }
      } catch (...) {
        err = "--fps 无效数值: " + args[i];
        return false;
      }
    } else if (arg == "--confidence") {
      if (i + 1 >= args.size()) {
        err = "选项 " + arg + " 缺少参数值";
        return false;
      }
      try {
        opts.confidence = std::stod(args[++i]);
        if (opts.confidence < 0.0 || opts.confidence > 1.0) {
          err = "--confidence 必须位于 [0.0, 1.0] 范围内";
          return false;
        }
      } catch (...) {
        err = "--confidence 无效数值: " + args[i];
        return false;
      }
    } else if (arg == "--bottom-ratio") {
      if (i + 1 >= args.size()) {
        err = "选项 " + arg + " 缺少参数值";
        return false;
      }
      try {
        opts.bottom_ratio = std::stod(args[++i]);
        if (opts.bottom_ratio <= 0.0 || opts.bottom_ratio >= 1.0) {
          err = "--bottom-ratio 必须位于 (0.0, 1.0) 范围内";
          return false;
        }
      } catch (...) {
        err = "--bottom-ratio 无效数值: " + args[i];
        return false;
      }
    } else if (arg == "--threads") {
      if (i + 1 >= args.size()) {
        err = "选项 " + arg + " 缺少参数值";
        return false;
      }
      try {
        int th = std::stoi(args[++i]);
        if (th < 0) {
          err = "--threads 不能为负数";
          return false;
        }
        opts.threads = static_cast<std::size_t>(th);
      } catch (...) {
        err = "--threads 无效数值: " + args[i];
        return false;
      }
    } else if (arg == "--model-dir") {
      if (i + 1 >= args.size()) {
        err = "选项 " + arg + " 缺少参数值";
        return false;
      }
      opts.model_dir = args[++i];
    } else if (arg == "--script") {
      if (i + 1 >= args.size()) {
        err = "选项 " + arg + " 缺少参数值";
        return false;
      }
      opts.script = args[++i];
      if (opts.script != "auto" && opts.script != "cjk" && opts.script != "latin") {
        err = "--script 仅支持 'auto', 'cjk', 'latin': " + opts.script;
        return false;
      }
    } else if (arg == "--ort-threads") {
      if (i + 1 >= args.size()) {
        err = "选项 " + arg + " 缺少参数值";
        return false;
      }
      try {
        int th = std::stoi(args[++i]);
        if (th <= 0) {
          err = "--ort-threads 必须为正整数";
          return false;
        }
        opts.ort_threads = th;
      } catch (...) {
        err = "--ort-threads 无效数值: " + args[i];
        return false;
      }
    } else if (arg == "--det-pixel-budget") {
      if (i + 1 >= args.size()) {
        err = "选项 " + arg + " 缺少参数值";
        return false;
      }
      try {
        opts.det_pixel_budget = std::stod(args[++i]);
        if (auto budget_err =
                sublift::adapters::paddle::validate_det_pixel_budget(opts.det_pixel_budget);
            budget_err.has_value()) {
          err = "--det-pixel-budget " + *budget_err;
          return false;
        }
      } catch (...) {
        err = "--det-pixel-budget 无效数值: " + args[i];
        return false;
      }
    } else if (arg == "--region") {
      if (i + 1 >= args.size()) {
        err = "选项 " + arg + " 缺少参数值";
        return false;
      }
      const std::string& reg_str = args[++i];
      int rx = 0;
      int ry = 0;
      int rw = 0;
      int rh = 0;
      char trailing = 0;
      if (std::sscanf(reg_str.c_str(), "%d,%d,%d,%d %c", &rx, &ry, &rw, &rh, &trailing) != 4 ||
          rw <= 0 || rh <= 0) {
        err = "--region 格式必须为 x,y,w,h 且宽高大于0: " + reg_str;
        return false;
      }
      opts.region_box = sublift::SourceBox{rx, ry, rw, rh};
    } else if (arg == "--auto-region") {
      opts.auto_region = true;
    } else if (arg == "--timing-only" || arg == "--pure-timing") {
      opts.timing_only = true;
    } else if (arg == "--hwaccel") {
      if (i + 1 >= args.size()) {
        err = "选项 " + arg + " 缺少参数值";
        return false;
      }
      std::string normalized;
      if (!is_supported_hwaccel(args[i + 1], normalized)) {
        err = "不支持的 --hwaccel 模式: " + args[i + 1] +
              " (可选: auto|none|videotoolbox|cuda|vaapi|qsv|d3d11va)";
        return false;
      }
      opts.hwaccel = std::move(normalized);
      ++i;
    } else if (arg == "--skip-frame") {
      if (i + 1 >= args.size()) {
        err = "选项 " + arg + " 缺少参数值";
        return false;
      }
      std::string normalized;
      if (!is_supported_skip_frame(args[i + 1], normalized)) {
        err = "不支持的 --skip-frame 模式: " + args[i + 1] +
              " (可选: auto|none|noref)";
        return false;
      }
      opts.skip_frame = std::move(normalized);
      ++i;
    } else if (arg == "--start-ms") {
      if (i + 1 >= args.size()) {
        err = "选项 " + arg + " 缺少参数值";
        return false;
      }
      std::int64_t ms = 0;
      if (!parse_strict_i64(args[++i], ms) || ms < 0) {
        err = "--start-ms 必须为非负整数毫秒值: " + args[i];
        return false;
      }
      opts.start_ms = ms;
    } else if (arg == "--end-ms") {
      if (i + 1 >= args.size()) {
        err = "选项 " + arg + " 缺少参数值";
        return false;
      }
      std::int64_t ms = 0;
      if (!parse_strict_i64(args[++i], ms) || ms <= 0) {
        err = "--end-ms 必须为正整数毫秒值: " + args[i];
        return false;
      }
      opts.end_ms = ms;
    } else if (arg == "--max-analysis-width") {
      if (i + 1 >= args.size()) {
        err = "选项 " + arg + " 缺少参数值";
        return false;
      }
      std::int64_t w = 0;
      if (!parse_strict_i64(args[++i], w) || w < 0 || w > 16384) {
        err = "--max-analysis-width 必须为 [0, 16384] 整数: " + args[i];
        return false;
      }
      opts.max_analysis_width = static_cast<std::int32_t>(w);
    } else if (arg == "--export-strips-dir") {
      if (i + 1 >= args.size()) {
        err = "选项 " + arg + " 缺少参数值";
        return false;
      }
      opts.export_strips_dir = args[++i];
      if (opts.export_strips_dir.empty()) {
        err = "--export-strips-dir 不能为空路径";
        return false;
      }
    } else if (!arg.empty() && arg[0] == '-') {
      err = "未知选项: " + arg;
      return false;
    } else {
      if (opts.video_path.empty()) {
        opts.video_path = arg;
      } else {
        err = "多余的位置参数: " + arg;
        return false;
      }
    }
  }

  if (opts.auto_region && opts.region_box.has_value()) {
    err = "选项 --auto-region 与 --region 互斥，请勿同时指定";
    return false;
  }

  if (opts.start_ms.has_value() && opts.end_ms.has_value() &&
      *opts.end_ms <= *opts.start_ms) {
    err = "--end-ms 必须严格大于 --start-ms";
    return false;
  }

  if (!opts.export_strips_dir.empty() && !opts.timing_only) {
    err = "选项 --export-strips-dir 必须与 --timing-only 配合使用";
    return false;
  }

  if (opts.video_path.empty()) {
    err = "缺少输入视频路径";
    return false;
  }

  return true;
}

class CliProgressObserver final : public sublift::IExtractionObserver {
 public:
  explicit CliProgressObserver(bool is_tty = true,
                               std::filesystem::path export_strips_dir = {})
      : is_tty_(is_tty), export_strips_dir_(std::move(export_strips_dir)) {}

  void on_segment_detected(const sublift::timing::Segment& seg) override {
    if (export_strips_dir_.empty() || seg.end_ms <= seg.start_ms || seg.anchor_image.empty()) {
      return;
    }
    const auto view = seg.anchor_image.view();
    if (view.width() <= 0 || view.height() <= 0) {
      return;
    }

    const int bpp = sublift::bytes_per_pixel(view.format());
    const char* magic = (view.format() == sublift::PixelFormat::RGB24) ? "P6" : "P5";
    const std::size_t row_bytes =
        static_cast<std::size_t>(view.width()) * static_cast<std::size_t>(bpp);

    const std::uint64_t seq = export_seq_.fetch_add(1, std::memory_order_relaxed);
#if defined(_WIN32)
    const long long pid_val = 0;
#else
    const long long pid_val = static_cast<long long>(::getpid());
#endif
    const std::string base_name =
        "strip_" + std::to_string(seg.start_ms) + "_" + std::to_string(seg.end_ms) + ".ppm";
    const std::string tmp_name =
        ".tmp_" + std::to_string(pid_val) + "_" + std::to_string(seq) + "_" + base_name;
    const auto tmp_path = export_strips_dir_ / tmp_name;
    const auto final_path = export_strips_dir_ / base_name;

    std::ofstream ofs(tmp_path, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
      return;
    }
    const std::string header =
        std::string(magic) + "\n" + std::to_string(view.width()) + " " +
        std::to_string(view.height()) + "\n255\n";
    ofs.write(header.data(), static_cast<std::streamsize>(header.size()));
    for (std::int32_t y = 0; y < view.height(); ++y) {
      ofs.write(reinterpret_cast<const char*>(view.row(y)),
                static_cast<std::streamsize>(row_bytes));
    }
    ofs.flush();
    const bool write_ok = !ofs.fail();
    ofs.close();

    std::error_code ec;
    if (write_ok) {
      std::filesystem::rename(tmp_path, final_path, ec);
      if (ec) {
        std::filesystem::remove(tmp_path, ec);
      }
    } else {
      std::filesystem::remove(tmp_path, ec);
    }
  }

  void on_progress(std::int64_t current_ms, std::int64_t total_ms, double fps) override {
    double pct = (total_ms > 0)
                     ? (static_cast<double>(current_ms) * 100.0 / static_cast<double>(total_ms))
                     : 0.0;
    pct = std::clamp(pct, 0.0, 100.0);

    auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::milliseconds>(now - last_progress_time_).count() < 150 &&
        pct < 100.0) {
      return;
    }
    last_progress_time_ = now;

    if (is_tty_) {
      std::fprintf(stdout, "\r[进度] %5.1f%% (%s / %s, %.1f fps)",
                   pct,
                   format_time(current_ms).c_str(),
                   format_time(total_ms).c_str(),
                   fps);
      std::fflush(stdout);
    } else {
      int cur_bucket = static_cast<int>(pct / 10.0);
      if (cur_bucket > last_bucket_) {
        last_bucket_ = cur_bucket;
        std::fprintf(stdout, "[进度] %5.1f%% (%s / %s)\n",
                     pct, format_time(current_ms).c_str(), format_time(total_ms).c_str());
        std::fflush(stdout);
      }
    }
  }

  void on_subtitle_entry(const sublift::SubtitleEntry& entry) override {
    if (is_tty_) {
      std::fprintf(stdout, "\r\033[K");
    }
    std::fprintf(stdout, "[字幕 #%llu] %s --> %s | %s (置信度: %.2f)\n",
                 static_cast<unsigned long long>(entry.id),
                 sublift::text::format_timestamp_srt(entry.range.start_ms).c_str(),
                 sublift::text::format_timestamp_srt(entry.range.end_ms).c_str(),
                 entry.text.c_str(),
                 entry.confidence);
    std::fflush(stdout);
  }

  void on_error(const sublift::ExtractionError& error) override {
    if (is_tty_) {
      std::fprintf(stdout, "\r\033[K");
    }
    std::fprintf(stderr, "[错误] 提取异常: %s (时间戳: %s)\n",
                 error.message.c_str(),
                 sublift::text::format_timestamp_srt(error.timestamp_ms).c_str());
  }

  void on_complete(const sublift::ExtractionMetrics& metrics) override {
    if (is_tty_) {
      std::fprintf(stdout, "\r\033[K");
    }
    std::fprintf(stdout,
                 "\n[完成] 耗时: %.2fs | 帧数: %llu | 候选片段: %llu | OCR调用: %llu | 生成字幕: %llu 条 | 吞吐: %.1f fps\n",
                 static_cast<double>(metrics.wall_time_ms) / 1000.0,
                 static_cast<unsigned long long>(metrics.total_frames_processed),
                 static_cast<unsigned long long>(metrics.total_segments_detected),
                 static_cast<unsigned long long>(metrics.total_ocr_calls),
                 static_cast<unsigned long long>(metrics.total_entries_produced),
                 metrics.effective_fps);
    std::fflush(stdout);
  }

 private:
  static std::string format_time(std::int64_t ms) {
    if (ms < 0) ms = 0;
    auto s = ms / 1000;
    auto m = s / 60;
    auto h = m / 60;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02lld:%02lld:%02lld",
                  static_cast<long long>(h),
                  static_cast<long long>(m % 60),
                  static_cast<long long>(s % 60));
    return buf;
  }

  bool is_tty_{true};
  std::filesystem::path export_strips_dir_{};
  std::atomic<std::uint64_t> export_seq_{0};
  int last_bucket_{-1};
  std::chrono::steady_clock::time_point last_progress_time_{};
};

}  // namespace

int main(int argc, char** argv) {
  // 入口首行建立 self-pipe 异步信号安全监听，覆盖参数校验、探测、模型加载与抽取全阶段
  SignalWatcher signal_watcher;

  CliOptions opts;
  std::string err;
  if (!parse_args(argc, argv, opts, err)) {
    std::fprintf(stderr, "参数错误: %s\n\n", err.c_str());
    print_usage(argv[0]);
    return 2;
  }

  if (opts.show_help) {
    print_usage(argv[0]);
    return 0;
  }

  if (opts.show_version) {
    print_version();
    return 0;
  }

  if (g_signal_received != 0) {
    std::fprintf(stdout, "\n提取已被用户取消。\n");
    return 130;
  }

  // 1. 验证 FFmpeg 工具可用性
  if (!sublift::adapters::ffmpeg::available()) {
    std::fprintf(stderr, "错误: 系统中未找到 ffmpeg 或 ffprobe 可执行文件，无法继续。\n");
    return 1;
  }

  // 2. 验证输入视频文件是否存在
  std::error_code ec;
  if (!std::filesystem::exists(opts.video_path, ec)) {
    std::fprintf(stderr, "错误: 输入视频文件不存在: %s\n", opts.video_path.string().c_str());
    return 1;
  }

  // 3. 初始化 FrameSource 与探测视频元数据
  sublift::adapters::ffmpeg::FfmpegFrameSource frame_source(opts.fps);
  frame_source.set_hwaccel(opts.hwaccel);
  signal_watcher.bind_source(&frame_source);
  sublift::VideoInfo video_info;
  try {
    video_info = frame_source.probe(opts.video_path);
  } catch (const std::exception& ex) {
    signal_watcher.unbind_all();
    std::fprintf(stderr, "错误: 探测视频元数据失败: %s\n", ex.what());
    return 1;
  }

  // 4. 初始化 PaddleOCR 引擎（纯打轴模式下无 --auto-region / --model-dir 时跳过模型校验与加载）
  std::unique_ptr<sublift::adapters::paddle::PaddleOcrEngine> paddle_engine;
  NullOcrEngine null_ocr;
  const bool need_paddle = !opts.timing_only || opts.auto_region || !opts.model_dir.empty();
  if (need_paddle) {
    sublift::adapters::paddle::PaddleOptions paddle_opts;
    paddle_opts.model_type = "small";
    if (!opts.model_dir.empty()) {
      paddle_opts.model_root_dir = opts.model_dir;
    }
    paddle_opts.intra_op_threads = static_cast<std::int32_t>(opts.ort_threads);
    paddle_opts.det_geometry.pixel_budget = opts.det_pixel_budget;
    paddle_opts.det_geometry.reference_width =
        std::min(2000.0, static_cast<double>(video_info.width));

    try {
      paddle_engine = std::make_unique<sublift::adapters::paddle::PaddleOcrEngine>(paddle_opts);
    } catch (const std::exception& ex) {
      signal_watcher.unbind_all();
      std::fprintf(stderr, "错误: 初始化 PaddleOCR 引擎失败: %s\n", ex.what());
      return 1;
    }
  }

  // 5. 配置 Session 与选择动态检测器
  sublift::ExtractionConfig config;
  config.sample_fps = opts.fps;
  config.confidence_threshold = opts.confidence;
  config.region_bottom_ratio = opts.bottom_ratio;
  config.num_threads = opts.threads;
  config.subtitle_script = opts.script;
  config.drop_empty_text = false;
  config.pure_timing = opts.timing_only;
  if (opts.timing_only) {
    config.min_duration_ms = 0;
    config.queue_capacity = std::max<std::size_t>(config.queue_capacity, 32);
    if (!opts.export_strips_dir.empty()) {
      std::error_code strip_dir_ec;
      std::filesystem::create_directories(opts.export_strips_dir, strip_dir_ec);
      if (strip_dir_ec) {
        signal_watcher.unbind_all();
        std::fprintf(stderr, "错误: 无法创建字幕条带导出目录 %s: %s\n",
                     opts.export_strips_dir.string().c_str(), strip_dir_ec.message().c_str());
        return 1;
      }
      config.retain_midpoint_in_pure_timing = true;
    }
  }

  std::optional<sublift::SourceBox> effective_region_box = opts.region_box;
  std::unique_ptr<sublift::IDetector> detector;
  if (opts.auto_region) {
    std::fprintf(stdout, "正在执行自动字幕区域前置探测...\n");
    std::fflush(stdout);
    {
      sublift::AutoRegionOptions auto_opts;
      auto_opts.script = opts.script;
      auto_opts.bottom_ratio = opts.bottom_ratio;

      auto detect_result = sublift::detect_auto_region(
          frame_source, *paddle_engine, opts.video_path, auto_opts);

      if (detect_result.detected) {
        std::fprintf(stdout, "自动区域探测成功: 区域=[x=%d, y=%d, w=%d, h=%d], 置信度=%.2f, 样例=\"%s\"\n",
                     detect_result.region_box.x, detect_result.region_box.y,
                     detect_result.region_box.width, detect_result.region_box.height,
                     detect_result.confidence, detect_result.preview_text.c_str());
        std::fflush(stdout);
        effective_region_box = detect_result.region_box;
        config.predetected_profile = detect_result.profile;
        sublift::FrameLocalBox box{detect_result.region_box.x, detect_result.region_box.y,
                                   detect_result.region_box.width, detect_result.region_box.height};
        detector = std::make_unique<sublift::adapters::ffmpeg::FixedRegionDetector>(box);
      } else {
        std::fprintf(stderr, "警告: 自动区域探测未检出明确字幕带 (%s)，平滑回退至底部 %.0f%% 兜底区域\n",
                     detect_result.error_message.c_str(), opts.bottom_ratio * 100.0);
        std::fflush(stderr);
        effective_region_box = std::nullopt;
        config.predetected_profile = std::nullopt;
        detector = std::make_unique<sublift::adapters::ffmpeg::BottomCropDetector>(opts.bottom_ratio);
      }
    }
    if (opts.timing_only) {
      paddle_engine.reset();
    }
  } else if (opts.region_box.has_value()) {
    sublift::FrameLocalBox box{opts.region_box->x, opts.region_box->y,
                               opts.region_box->width, opts.region_box->height};
    detector = std::make_unique<sublift::adapters::ffmpeg::FixedRegionDetector>(box);
  } else {
    detector = std::make_unique<sublift::adapters::ffmpeg::BottomCropDetector>(opts.bottom_ratio);
  }

  // 在 detect_auto_region 完成全片关键帧扫描后，再设置分析阶段的切片区间与分辨率上限
  if (opts.start_ms.has_value() || opts.end_ms.has_value()) {
    const std::optional<double> start_sec =
        opts.start_ms.has_value()
            ? std::optional<double>(static_cast<double>(*opts.start_ms) / 1000.0)
            : std::nullopt;
    std::optional<double> duration_sec = std::nullopt;
    if (opts.end_ms.has_value()) {
      const std::int64_t begin_ms = opts.start_ms.value_or(0);
      duration_sec = static_cast<double>(*opts.end_ms - begin_ms) / 1000.0;
    }
    frame_source.set_time_range(start_sec, duration_sec);
  }
  std::int32_t effective_max_width =
      (opts.max_analysis_width >= 0) ? opts.max_analysis_width
                                     : (opts.timing_only ? 1920 : 0);
  if (effective_region_box.has_value() && !video_info.display_transform_ok &&
      effective_max_width > 0) {
    // 非恒等显示变换下 FallbackFull 由 FixedRegionDetector 按源坐标裁剪，禁止提前缩放
    effective_max_width = 0;
  }
  if (!opts.timing_only && effective_max_width > 0 &&
      video_info.width > effective_max_width) {
    config.predetected_profile = std::nullopt;
  }
  auto extractor_options = frame_source.options();
  extractor_options.max_output_width = effective_max_width;
  if (!opts.skip_frame.empty()) {
    extractor_options.skip_frame = opts.skip_frame;
  }
  frame_source.set_options(extractor_options);

  sublift::IOcrEngine& active_ocr = opts.timing_only
                                        ? static_cast<sublift::IOcrEngine&>(null_ocr)
                                        : static_cast<sublift::IOcrEngine&>(*paddle_engine);
  sublift::ExtractionSession session(frame_source, active_ocr, detector.get(), nullptr, config);
  signal_watcher.bind_session(&session);
  if (g_signal_received != 0) {
    session.cancel();
  }

  const bool is_tty = (isatty_compat(fileno_compat(stdout)) != 0);
  CliProgressObserver observer(is_tty, opts.export_strips_dir);
  session.set_observer(&observer);

  std::fprintf(stdout, "开始处理视频: %s\n", opts.video_path.string().c_str());
  std::fprintf(stdout,
               "配置参数: fps=%.1f, 置信度阈值=%.2f, 底部裁剪比=%.2f, 线程数=%zu, ORT线程数=%d, 像素预算=%.0f, hwaccel=%s, skip_frame=%s, 最大分析宽度=%d%s\n",
               opts.fps, opts.confidence, opts.bottom_ratio, opts.threads, opts.ort_threads,
               opts.det_pixel_budget, opts.hwaccel.c_str(),
               opts.skip_frame.empty() ? "auto" : opts.skip_frame.c_str(),
               effective_max_width,
               opts.timing_only ? ", 模式=纯打轴(--timing-only)" : "");
  std::fflush(stdout);

  // 6. 执行视频抽取（信号中断由 SignalWatcher self-pipe 线程安全唤醒 session.cancel()）
  std::vector<sublift::SubtitleEntry> entries;
  try {
    entries = session.process_video(opts.video_path, effective_region_box);
  } catch (const std::exception& ex) {
    signal_watcher.unbind_all();
    std::fprintf(stderr, "\n提取异常终止: %s\n", ex.what());
    return 1;
  }

  signal_watcher.unbind_all();

  if (session.is_cancelled() || g_signal_received != 0) {
    std::fprintf(stdout, "\n提取已被用户取消。\n");
    return 130;
  }

  // 7. 格式化并输出 SRT 或 JSONL
  std::string output_content;
  std::size_t cue_count = 0;
  if (opts.timing_only && opts.output_path.extension() == ".jsonl") {
    std::string jsonl;
    jsonl.reserve(entries.size() * 64);
    for (const auto& entry : entries) {
      if (entry.range.end_ms <= entry.range.start_ms) {
        continue;
      }
      ++cue_count;
      char buf[160];
      const int n = std::snprintf(
          buf, sizeof(buf), "{\"index\":%zu,\"start_ms\":%lld,\"end_ms\":%lld}\n",
          cue_count, static_cast<long long>(entry.range.start_ms),
          static_cast<long long>(entry.range.end_ms));
      if (n > 0) {
        jsonl.append(buf, static_cast<std::size_t>(n));
      }
    }
    output_content = std::move(jsonl);
  } else {
    if (opts.timing_only) {
      for (auto& entry : entries) {
        entry.text = "TIMING_ONLY";
      }
    }
    auto [srt_content, count] = sublift::text::format_srt_counted(entries);
    output_content = std::move(srt_content);
    cue_count = count;
  }

  if (opts.output_path.has_parent_path()) {
    std::error_code dir_ec;
    std::filesystem::create_directories(opts.output_path.parent_path(), dir_ec);
  }

  std::ofstream ofs(opts.output_path, std::ios::binary | std::ios::trunc);
  if (!ofs.is_open()) {
    std::fprintf(stderr, "错误: 无法写入输出文件: %s\n", opts.output_path.string().c_str());
    return 1;
  }
  ofs << output_content;
  ofs.close();

  if (opts.timing_only) {
    std::fprintf(stdout, "打轴输出成功: %s (共 %zu 条字幕时间段)\n",
                 opts.output_path.string().c_str(), cue_count);
  } else {
    std::fprintf(stdout, "字幕输出成功: %s (共 %zu 条有效字幕)\n",
                 opts.output_path.string().c_str(), cue_count);
  }

  return 0;
}
