#pragma once

#include <cstdint>
#include <sublift/core/image_buffer.hpp>
#include <sublift/core/image_view.hpp>
#include <sublift/timing/timing_config.hpp>

namespace sublift::timing {

/// Visual analysis result for a single frame, prepared ahead of serialized
/// state machine processing (changepoint/timeline).
///
/// Fully decoupled from sublift/features in public contracts.
/// Aggregate struct with move-only semantics inherited from ImageBuffer.
struct AnalyzedFrame {
  std::uint64_t frame_index{0};
  std::int64_t timestamp_ms{0};
  std::uint64_t dhash64{0};
  float presence_ratio{0.0f};
  bool has_subtitle{false};
  ImageBuffer roi_image{};
  ImageBuffer roi_mask{};
  bool is_cut{false};

  [[nodiscard]] double foreground_ratio() const noexcept {
    return static_cast<double>(presence_ratio);
  }

  [[nodiscard]] AnalyzedFrame clone() const {
    return AnalyzedFrame{
        .frame_index = frame_index,
        .timestamp_ms = timestamp_ms,
        .dhash64 = dhash64,
        .presence_ratio = presence_ratio,
        .has_subtitle = has_subtitle,
        .roi_image = roi_image.clone(),
        .roi_mask = roi_mask.clone(),
        .is_cut = is_cut,
    };
  }
};

/// Pure visual analysis helper executing mask computation, presence ratio,
/// and 64-bit dHash.
///
/// Thread-safe and stateless. Suitable for execution in worker thread pools.
/// If config.pure_timing is true and retain_midpoint_in_pure_timing is false,
/// roi_image is not retained to save memory.
[[nodiscard]] AnalyzedFrame analyze_frame(
    const ImageView& frame,
    const TimingConfig& config,
    std::uint64_t frame_index,
    std::int64_t timestamp_ms);

/// Overload taking an owned ImageBuffer&& to avoid pixel copies when possible.
[[nodiscard]] AnalyzedFrame analyze_frame(
    ImageBuffer&& frame,
    const TimingConfig& config,
    std::uint64_t frame_index,
    std::int64_t timestamp_ms);

} // namespace sublift::timing
