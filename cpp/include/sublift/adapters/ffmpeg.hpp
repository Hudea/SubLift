#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json_fwd.hpp>

#include "sublift/extractor.hpp"
#include "sublift/models.hpp"

namespace sublift::ffmpeg {

enum class TransformPolicy {
  FallbackFull,
  Error
};

/// 检查系统当前 ffmpeg 和 ffprobe 是否均可用（无异常抛出）
[[nodiscard]] bool available() noexcept;

/// 解析 ffmpeg 可执行文件绝对路径。未找到抛 std::runtime_error（附带尝试过的路径列表）
[[nodiscard]] std::string resolve_ffmpeg_bin();

/// 解析 ffprobe 可执行文件绝对路径。未找到抛 std::runtime_error（附带尝试过的路径列表）
[[nodiscard]] std::string resolve_ffprobe_bin();

/// 严格校验 source-frame output_crop；非法抛 std::invalid_argument，消息含 source 与 requested
void validate_output_crop(const SourceBox& crop, std::int32_t source_width,
                            std::int32_t source_height);

/// 构造 ffmpeg `-vf` 滤镜链字符串（字符级对齐 Python build_output_vf）
[[nodiscard]] std::string build_output_vf(double fps, const SourceBox* crop);
[[nodiscard]] std::string build_output_vf(double fps,
                                          const std::optional<SourceBox>& crop);

/// 判断显示/旋转变换是否恒等且可信任（与 Python _assess_display_transform 对齐）
/// stream_json 为 ffprobe JSON streams[0] 节点
[[nodiscard]] std::pair<bool, std::optional<std::string>> assess_display_transform(
    const nlohmann::json& stream_json);

/// 尽力识别恒等 Display Matrix（与 Python _is_identity_display_matrix 对齐）
/// side_data_json 为 side_data_list 中的单条记录
[[nodiscard]] bool is_identity_display_matrix(const nlohmann::json& side_data_json);

/// 探测 source-frame 尺寸与显示变换状态（调用 ffprobe -select_streams v:0 ...）
/// 超时 60 秒，失败抛 std::runtime_error（附带 stderr 尾部摘要）
[[nodiscard]] SourceFrameInfo probe_source_frame(const std::filesystem::path& video_path);

/// 探测视频时长（毫秒）。文件不存在或 ffprobe 探测失败返回 0（与 Python probe_duration_ms 一致）
[[nodiscard]] std::int64_t probe_duration_ms(const std::filesystem::path& video_path);

/// 探测视频综合元数据（组合 probe_source_frame 与 probe_duration_ms）
[[nodiscard]] VideoInfo probe_video(const std::filesystem::path& video_path);

/// 从视频中按指定秒数抽取单帧 JPEG 图像数据（供 Web 服务 / 预览直接使用）
/// @param video_path 视频绝对路径
/// @param time_seconds 指定截取时间点（秒），默认 0.0
/// @param crop 可选的裁剪区域（SourceBox）
/// @param quality JPEG 质量（1-31，数值越小质量越高，默认 2）
[[nodiscard]] std::vector<std::uint8_t> extract_single_frame_jpeg(
    const std::filesystem::path& video_path,
    double time_seconds = 0.0,
    const std::optional<SourceBox>& crop = std::nullopt,
    int quality = 2);

/// 将非浏览器原生支持的视频（MKV, AVI, FLV, ProRes MOV 等）极速转封装/转码为带 faststart 的 MP4 预览缓存文件。
/// 若输入已是兼容的 MP4 且无需转封装，或缓存中已有有效产物，直接返回路径。
[[nodiscard]] std::filesystem::path remux_to_faststart_mp4(
    const std::filesystem::path& video_path,
    const std::optional<std::filesystem::path>& custom_cache_dir = std::nullopt);

/// 纯规划：不调用 ffprobe。`source` 在 mode 为 auto|roi 且有 region_box 时必需。
/// 与 Python plan_frame_io 在已 mock probe 时的分支语义一致（测试 / golden 用）。
[[nodiscard]] FrameIOPlan plan_frame_io_pure(
    const std::optional<SourceBox>& region_box,
    std::string_view mode = "auto",
    const std::optional<SourceFrameInfo>& source = std::nullopt,
    TransformPolicy on_unvalidated_transform = TransformPolicy::FallbackFull,
    double bottom_ratio = 0.3);

/// 根据 region 与模式规划 output_crop + detector（与 Python plan_frame_io 行为 parity）
/// ROI 路径内部 probe 后委派 plan_frame_io_pure。
[[nodiscard]] FrameIOPlan plan_frame_io(
    const std::filesystem::path& video_path,
    const std::optional<SourceBox>& region_box,
    std::string_view mode = "auto",
    TransformPolicy on_unvalidated_transform = TransformPolicy::FallbackFull,
    double bottom_ratio = 0.3);

/// 通过 ffmpeg subprocess 按 fps 抽帧的 Extractor 实现（支持 cancel 协作取消）
class FfmpegExtractor final : public IExtractor {
 public:
  explicit FfmpegExtractor(double fps = 1.0,
                           std::optional<SourceBox> output_crop = std::nullopt,
                           std::optional<SourceFrameInfo> source_info = std::nullopt);
  ~FfmpegExtractor() override;

  FfmpegExtractor(const FfmpegExtractor&) = delete;
  FfmpegExtractor& operator=(const FfmpegExtractor&) = delete;
  FfmpegExtractor(FfmpegExtractor&&) = delete;
  FfmpegExtractor& operator=(FfmpegExtractor&&) = delete;

  /// 从视频按 fps 抽帧，按帧回调 consumer 消费。
  /// consumer 返回 false 时提前终止抽帧。
  void extract(const std::filesystem::path& video_path,
               const std::function<bool(Frame)>& consumer);

  /// 强制/协作式取消抽帧：设置原子标志并终止 kill 子进程
  void cancel() override;

  /// 查询当前 output_crop（全帧模式返回 std::nullopt）
  [[nodiscard]] std::optional<SourceBox> output_crop() const override { return output_crop_; }

  /// 获取配置的 fps
  [[nodiscard]] double fps() const noexcept { return fps_; }

  /// 查询是否已被取消
  [[nodiscard]] bool is_cancelled() const noexcept { return cancelled_.load(std::memory_order_relaxed); }

 private:
  double fps_{1.0};
  std::optional<SourceBox> output_crop_{std::nullopt};
  std::optional<SourceFrameInfo> source_info_{std::nullopt};

  std::atomic<bool> cancelled_{false};
  mutable std::mutex proc_mutex_{};
  pid_t active_pid_{-1};
};

}  // namespace sublift::ffmpeg
