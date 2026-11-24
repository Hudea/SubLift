#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sublift/core/box.hpp>
#include <sublift/core/frame.hpp>
#include <sublift/core/image_buffer.hpp>
#include <sublift/ports/detector.hpp>
#include <sublift/ports/frame_source.hpp>
#include <sublift/adapters/ffmpeg/bottom_crop_detector.hpp>
#include <sublift/adapters/ffmpeg/fixed_detector.hpp>
#include <sublift/adapters/ffmpeg/roi_passthrough_detector.hpp>

namespace sublift::adapters::ffmpeg {

enum class TransformPolicy {
  FallbackFull,
  Error
};

/// 检查系统当前 ffmpeg 和 ffprobe 是否均可用（无异常抛出）
[[nodiscard]] bool available() noexcept;

/// 解析 ffmpeg 可执行文件绝对路径。未找到抛 std::runtime_error
[[nodiscard]] std::string resolve_ffmpeg_bin();

/// 解析 ffprobe 可执行文件绝对路径。未找到抛 std::runtime_error
[[nodiscard]] std::string resolve_ffprobe_bin();

/// 严格校验 source-frame output_crop；非法抛 std::invalid_argument
void validate_output_crop(const SourceBox& crop, std::int32_t source_width,
                          std::int32_t source_height);

/// 两阶段粗剪默认外扩边距（像素）
inline constexpr std::int32_t kDefaultTwoStageCropMargin = 8;

/// 两阶段裁剪计划元数据
struct TwoStageCropPlan {
  SourceBox coarse_crop;
  SourceBox exact_crop;
};

/// 计算两阶段粗剪几何与二次裁剪偏移（纯函数，无 ffmpeg 进程调用）
[[nodiscard]] TwoStageCropPlan plan_two_stage_crop(
    const SourceBox& crop,
    std::int32_t source_width,
    std::int32_t source_height,
    std::int32_t margin = kDefaultTwoStageCropMargin);

/// 构造两阶段优化 `-vf` 滤镜链字符串
[[nodiscard]] std::string build_optimized_output_vf(
    double fps,
    const std::optional<SourceBox>& crop,
    std::int32_t source_width,
    std::int32_t source_height,
    std::int32_t margin = kDefaultTwoStageCropMargin);

/// 构造单阶段基准 ffmpeg `-vf` 滤镜链字符串
[[nodiscard]] std::string build_output_vf(double fps, const SourceBox* crop);
[[nodiscard]] std::string build_output_vf(double fps,
                                          const std::optional<SourceBox>& crop);

/// 探测 source-frame 尺寸与显示变换状态
[[nodiscard]] SourceFrameInfo probe_source_frame(const std::filesystem::path& video_path);

/// 探测相对播放时长（毫秒）
[[nodiscard]] std::int64_t probe_duration_ms(const std::filesystem::path& video_path);

/// 合并探测：一次 ffprobe 同时拿到 source-frame 几何/显示变换、流编解码提示与时长
struct SourceAndDuration {
  SourceFrameInfo source;
  std::int64_t duration_ms{0};
  std::int64_t format_start_ms{0};
  double fps{0.0};
  std::string codec_name;
  std::string pix_fmt;
};
[[nodiscard]] SourceAndDuration probe_source_and_duration(
    const std::filesystem::path& video_path);

/// 探测视频综合元数据（组合 source_frame 几何、fps 与 duration）
[[nodiscard]] VideoInfo probe_video(const std::filesystem::path& video_path);

/// 从视频中按指定秒数抽取单帧 RGB24 内存图像数据
[[nodiscard]] ImageBuffer extract_single_frame_rgb24(
    const std::filesystem::path& video_path,
    double time_seconds = 0.0,
    int max_dimension = 1920);

/// 纯规划：不调用 ffprobe
[[nodiscard]] FrameIOPlan plan_frame_io_pure(
    const std::optional<SourceBox>& region_box,
    std::string_view mode = "auto",
    const std::optional<SourceFrameInfo>& source = std::nullopt,
    TransformPolicy on_unvalidated_transform = TransformPolicy::FallbackFull,
    double bottom_ratio = 0.3);

/// 根据 region 与模式规划 output_crop
[[nodiscard]] FrameIOPlan plan_frame_io(
    const std::filesystem::path& video_path,
    const std::optional<SourceBox>& region_box,
    std::string_view mode = "auto",
    TransformPolicy on_unvalidated_transform = TransformPolicy::FallbackFull,
    double bottom_ratio = 0.3);

/// 根据 plan 与 region_box 解析对应的检测器实例
[[nodiscard]] inline std::shared_ptr<IDetector> resolve_detector(
    const FrameIOPlan& plan,
    const std::optional<SourceBox>& region_box,
    double bottom_ratio = 0.3) {
  if (plan.output_mode == FrameOutputMode::RoiRgb && plan.output_crop.has_value()) {
    return std::make_shared<RoiPassthroughDetector>(
        plan.output_crop->width, plan.output_crop->height);
  }
  if (!region_box.has_value()) {
    return std::make_shared<BottomCropDetector>(bottom_ratio);
  }
  return std::make_shared<FixedRegionDetector>(
      FrameLocalBox{region_box->x, region_box->y, region_box->width, region_box->height});
}

/// FfmpegFrameSource 配置选项
inline constexpr double kAutoSkipNorefMinSourceFps = 49.5;
inline constexpr double kAutoSkipNorefMinFpsRatio = 3.0;

struct FfmpegExtractorOptions {
  bool enable_two_stage_crop{true};
  std::int32_t crop_margin{kDefaultTwoStageCropMargin};
  std::int32_t max_output_width{0};
  std::string skip_frame{};
};

/// 用于智能硬件解码与滤镜决策的流元数据提示
struct StreamDecodeHints {
  std::int32_t width{0};
  std::int32_t height{0};
  double fps{0.0};
  std::string codec_name;
  std::string pix_fmt;
};

/// 带分辨率上限规划的输出尺寸与 ffmpeg `-vf` 字符串
struct ScaledOutputVfPlan {
  std::string vf;
  std::int32_t out_width{0};
  std::int32_t out_height{0};
  bool scaled{false};
  bool used_nv12_bridge{false};
};

/// 判断像素格式是否为高色深（>8bit），用于插入 nv12 NEON 快速转换桥
[[nodiscard]] bool is_high_bit_depth_pix_fmt(std::string_view pix_fmt);

/// 解析实际传递给 ffmpeg `-hwaccel` 的模式（空串表示纯软件解码）。
/// 支持 "auto" 基于编解码器/色深/分辨率/帧率与当前平台选择（macOS 返回 "videotoolbox" 或 ""）。
[[nodiscard]] std::string resolve_effective_hwaccel(
    std::string_view requested_hwaccel,
    const StreamDecodeHints& hints);

/// 解析实际传递给 ffmpeg `-skip_frame` 的模式（空串表示完整解码所有帧）。
/// 支持 "auto"（仅当 hints.fps >= 49.5 且 sample_fps > 0 且 hints.fps >= sample_fps * 3.0 时返回 "noref"）、
/// "none"/"off"/"false"/"0"（返回 ""）、"noref"/"nonref"（归一化返回 "noref"）。
/// 非法模式抛出 std::invalid_argument。
[[nodiscard]] std::string resolve_effective_skip_frame(
    std::string_view requested_skip_frame,
    const StreamDecodeHints& hints,
    double sample_fps);

/// 在保留现有 build_output_vf / build_optimized_output_vf 契约的前提下，
/// 按 max_output_width 与 hw_mode / pix_fmt 规划缩放与 nv12 桥接滤镜链。
[[nodiscard]] ScaledOutputVfPlan plan_scaled_output_vf(
    double fps,
    const std::optional<SourceBox>& crop,
    std::int32_t source_width,
    std::int32_t source_height,
    const FfmpegExtractorOptions& options,
    std::string_view hw_mode = "",
    std::string_view pix_fmt = "");

/// 基于 FFmpeg 子进程与管道调用的 IFrameSource 实现
class FfmpegFrameSource : public IFrameSource {
 public:
  explicit FfmpegFrameSource(double fps = 1.0,
                             std::optional<SourceBox> output_crop = std::nullopt,
                             std::optional<SourceFrameInfo> source_info = std::nullopt,
                             std::optional<std::string> hwaccel = std::nullopt,
                             FfmpegExtractorOptions options = {});
  ~FfmpegFrameSource() override;

  FfmpegFrameSource(const FfmpegFrameSource&) = delete;
  FfmpegFrameSource& operator=(const FfmpegFrameSource&) = delete;
  FfmpegFrameSource(FfmpegFrameSource&&) = delete;
  FfmpegFrameSource& operator=(FfmpegFrameSource&&) = delete;

  // IFrameSource 纯虚接口实现
  [[nodiscard]] VideoInfo probe(const std::filesystem::path& video_path) override;

  [[nodiscard]] FrameIOPlan plan(
      const VideoInfo& info,
      const std::optional<SourceBox>& requested_box) override;

  void extract(
      const std::filesystem::path& video_path,
      double fps,
      const std::optional<SourceBox>& crop,
      std::function<bool(Frame)> consumer) override;

  [[nodiscard]] ImageBuffer extract_frame(
      const std::filesystem::path& video_path,
      std::int64_t timestamp_ms,
      const std::optional<SourceBox>& crop) override;

  [[nodiscard]] std::vector<KeyframeThumbnail> sample_keyframe_thumbnails(
      const std::filesystem::path& video_path,
      double bottom_ratio = 0.45,
      std::size_t max_samples = 60) override;

  void cancel() override;
  [[nodiscard]] bool is_cancelled() const noexcept override;

  // 既有测试与便捷重载
  void extract(const std::filesystem::path& video_path,
               const std::function<bool(Frame)>& consumer);

  [[nodiscard]] std::optional<SourceBox> output_crop() const { return output_crop_; }
  [[nodiscard]] double fps() const noexcept { return fps_; }
  [[nodiscard]] const FfmpegExtractorOptions& options() const noexcept { return options_; }
  void set_options(FfmpegExtractorOptions options) noexcept { options_ = std::move(options); }
  [[nodiscard]] std::int32_t crop_margin() const noexcept { return options_.crop_margin; }

  [[nodiscard]] const std::optional<std::string>& hwaccel() const noexcept { return hwaccel_; }
  void set_hwaccel(std::optional<std::string> hwaccel) { hwaccel_ = std::move(hwaccel); }

  [[nodiscard]] std::string_view skip_frame() const noexcept {
    return options_.skip_frame.empty() ? std::string_view("auto")
                                       : std::string_view(options_.skip_frame);
  }
  void set_skip_frame(std::string skip_frame) { options_.skip_frame = std::move(skip_frame); }

  void set_time_range(std::optional<double> start_sec,
                      std::optional<double> duration_sec = std::nullopt);
  [[nodiscard]] std::optional<double> start_sec() const noexcept { return start_sec_; }
  [[nodiscard]] std::optional<double> duration_sec() const noexcept { return duration_sec_; }

 private:
  double fps_{1.0};
  std::optional<SourceBox> output_crop_{std::nullopt};
  std::optional<SourceFrameInfo> source_info_{std::nullopt};
  std::optional<std::filesystem::path> probed_video_path_{std::nullopt};
  std::optional<VideoInfo> cached_video_info_{std::nullopt};
  std::optional<StreamDecodeHints> cached_stream_hints_{std::nullopt};
  std::optional<std::string> hwaccel_{std::nullopt};
  FfmpegExtractorOptions options_{};
  std::optional<double> start_sec_{std::nullopt};
  std::optional<double> duration_sec_{std::nullopt};

  [[nodiscard]] StreamDecodeHints ensure_probed_hints(
      const std::filesystem::path& video_path, double fallback_fps = 0.0);
  [[nodiscard]] std::string resolve_requested_hwaccel() const;
  [[nodiscard]] std::string resolve_requested_skip_frame() const;

  std::atomic<bool> cancelled_{false};
  mutable std::mutex proc_mutex_{};
  std::vector<pid_t> active_pids_{};
};

}  // namespace sublift::adapters::ffmpeg
