#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include "sublift/models.hpp"

namespace sublift {

/// Frame signature config (matches src/sublift/config.py SignatureConfig).
struct SignatureConfig {
  double block_size_ratio{0.08};
  std::int32_t adaptive_c{12};
  std::int32_t hash_size{8};

  friend constexpr bool operator==(const SignatureConfig&,
                                   const SignatureConfig&) = default;
};

/// Change-point detector config (matches ChangePointConfig).
struct ChangePointConfig {
  double presence_threshold{0.01};
  std::int32_t hysteresis_frames{1};
  std::int32_t change_threshold{10};
  bool enable_ssim_verify{false};
  double ssim_threshold{0.95};
  std::int32_t ssim_window_size{7};
  bool enable_ssim_patrol{true};
  std::int32_t ssim_patrol_interval{3};
  double ssim_patrol_threshold{0.92};
  bool ssim_patrol_use_mask{true};

  friend constexpr bool operator==(const ChangePointConfig&,
                                   const ChangePointConfig&) = default;
};

/// Full extract pipeline config (matches Config in config.py).
/// Top-level field count: 16 (keep in sync with Python).
struct Config {
  double sample_fps{5.0};
  double region_bottom_ratio{0.3};
  double confidence_threshold{0.5};
  std::int32_t merge_gap_ms{1000};
  std::int32_t min_duration_ms{500};
  std::int32_t ocr_anchor_delay_frames{2};
  bool drop_empty_text{false};
  std::optional<SubtitleProfile> subtitle_profile{std::nullopt};
  std::string subtitle_script{std::string{SCRIPT_AUTO}};
  bool enable_line_select{true};
  double low_conf_threshold{0.28};
  std::int32_t ocr_consensus_frames{4};
  double line_select_min_score{0.28};
  double line_select_min_script{0.12};
  SignatureConfig signature{};
  ChangePointConfig change_point{};

  friend bool operator==(const Config&, const Config&) = default;
};

inline const Config kDefaultConfig{};

}  // namespace sublift
