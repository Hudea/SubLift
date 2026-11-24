#pragma once

#include <cstdint>

namespace sublift::timing {

struct TimingConfig {
  std::int64_t hysteresis_ms{200};
  std::int64_t confirm_ms{200};
  std::int64_t patrol_interval_ms{600};
  std::int32_t change_threshold{10};
  double presence_threshold{0.01};
  bool enable_ssim_patrol{true};
  double ssim_patrol_threshold{0.92};
  bool ssim_patrol_use_mask{true};
  bool enable_ssim_verify{false};
  double ssim_threshold{0.95};
  int ssim_window_size{7};
  double aspect_ratio_threshold{6.0};
  std::int64_t ocr_anchor_delay_ms{400};
  std::int32_t ocr_consensus_frames{4};
  bool pure_timing{false};
  bool retain_midpoint_in_pure_timing{false};

  friend bool operator==(const TimingConfig&, const TimingConfig&) = default;
};

} // namespace sublift::timing
