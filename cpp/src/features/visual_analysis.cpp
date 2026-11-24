#include <sublift/features/dhash.hpp>
#include <sublift/features/mask.hpp>
#include <sublift/features/visual_analysis.hpp>
#include "adaptive_presence_mask.hpp"
#include "opencv_thread_guard.hpp"

namespace sublift::features {

namespace {

inline bool is_wide_subtitle_strip(const ImageView& view, double aspect_threshold) noexcept {
  if (view.empty() || view.height() <= 0) {
    return false;
  }
  const double aspect =
      static_cast<double>(view.width()) / static_cast<double>(view.height());
  return aspect > aspect_threshold;
}

} // namespace

FrameVisualAnalysis compute_visual_analysis(const ImageView& image, std::int64_t timestamp_ms,
                                           const MaskConfig& config) {
  detail::ensure_opencv_single_thread();
  if (image.empty()) {
    return FrameVisualAnalysis{
        .signature = FrameSignature{.timestamp_ms = timestamp_ms, .foreground_ratio = 0.0, .dhash = 0},
        .valid_mask = ImageBuffer{},
    };
  }

  if (is_wide_subtitle_strip(image, config.aspect_ratio_threshold) && config.use_adaptive_mask) {
    auto res = compute_adaptive_presence_mask(image, config);
    return FrameVisualAnalysis{
        .signature = FrameSignature{
            .timestamp_ms = timestamp_ms,
            .foreground_ratio = res.foreground_ratio,
            .dhash = res.dhash,
        },
        .valid_mask = std::move(res.valid_mask),
    };
  }

  ImageBuffer mask = compute_binary_mask(image, config);
  double fg_ratio = compute_foreground_ratio(mask.view());
  std::uint64_t hash = compute_dhash(mask.view());

  return FrameVisualAnalysis{
      .signature = FrameSignature{
          .timestamp_ms = timestamp_ms,
          .foreground_ratio = fg_ratio,
          .dhash = hash,
      },
      .valid_mask = std::move(mask),
  };
}

} // namespace sublift::features
