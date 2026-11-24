#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include <sublift/core/models.hpp>

namespace sublift {

struct ExtractionConfig {
  // 采样参数
  double sample_fps{5.0};
  double region_bottom_ratio{0.3};

  // 打轴状态机参数（纯毫秒基准）
  std::int64_t hysteresis_ms{200};
  std::int64_t confirm_ms{200};
  std::int64_t patrol_interval_ms{600};
  std::int32_t change_threshold{10};
  double presence_threshold{0.01};
  bool enable_ssim_patrol{true};
  double ssim_patrol_threshold{0.92};
  double aspect_ratio_threshold{6.0};  // 统一跨层宽条字幕带几何阈值 (width / height > threshold，跨 features/timing/text 三层生效)

  // 文本筛选与共识参数
  bool enable_line_select{true};
  double confidence_threshold{0.5};
  double low_conf_threshold{0.28};
  double line_select_min_score{0.28};
  double line_select_min_script{0.12};
  std::int64_t ocr_anchor_delay_ms{400};
  std::int32_t ocr_consensus_frames{4};
  std::int64_t min_duration_ms{500};
  std::int64_t merge_gap_ms{1000};
  std::string subtitle_script{"auto"};
  bool drop_empty_text{false};

  // 预探测 Profile 注入支持 (Feature 10)
  std::optional<SubtitleProfile> predetected_profile{std::nullopt};

  // 调度并发控制
  std::size_t num_threads{0};    // 0 表示自适应系统物理核心数
  std::size_t queue_capacity{8};  // 流水线背压深度（由早期 16 收紧为 8，平衡并行吞吐与在途 RGB 帧峰值内存）
  bool pure_timing{false};       // 纯打轴模式（不保留图像以节约内存）
  bool retain_midpoint_in_pure_timing{false};  // 纯打轴模式下为单遍条带导出仅保留中点代表帧

  friend bool operator==(const ExtractionConfig&, const ExtractionConfig&) = default;
};

} // namespace sublift
