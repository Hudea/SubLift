#include <sublift/timing/frame_analysis.hpp>

#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>

#include <sublift/core/color.hpp>
#include <sublift/features/dhash.hpp>
#include <sublift/features/mask.hpp>
#include <sublift/features/visual_analysis.hpp>

namespace sublift::timing {

namespace {

ImageBuffer copy_view_to_buffer(const ImageView& view) {
  if (view.empty()) {
    return ImageBuffer{};
  }
  ImageBuffer buf = ImageBuffer::allocate_uninitialized(
      view.width(), view.height(), view.format());
  const int bpp = bytes_per_pixel(view.format());
  const std::size_t row_bytes =
      static_cast<std::size_t>(view.width()) * static_cast<std::size_t>(bpp);
  for (std::int32_t y = 0; y < view.height(); ++y) {
    const auto* src = view.row(y);
    auto* dst = buf.row(y);
    std::memcpy(dst, src, row_bytes);
  }
  return buf;
}

bool check_has_subtitle(double foreground_ratio, std::uint64_t dhash,
                        std::int32_t width, std::int32_t height,
                        const TimingConfig& config) {
  if (width <= 0 || height <= 0) {
    return false;
  }
  const double aspect = static_cast<double>(width) / static_cast<double>(height);
  const bool wide = (aspect > config.aspect_ratio_threshold);
  return (foreground_ratio >= config.presence_threshold) &&
         (!wide || dhash != 0ULL);
}

} // namespace

AnalyzedFrame analyze_frame(
    const ImageView& frame,
    const TimingConfig& config,
    std::uint64_t frame_index,
    std::int64_t timestamp_ms) {
  if (frame.empty()) {
    return AnalyzedFrame{
        .frame_index = frame_index,
        .timestamp_ms = timestamp_ms,
        .dhash64 = 0ULL,
        .presence_ratio = 0.0f,
        .has_subtitle = false,
        .roi_image = ImageBuffer{},
        .roi_mask = ImageBuffer{},
        .is_cut = false,
    };
  }

  // Stateless visual feature computation via sublift/features
  features::MaskConfig mask_cfg{};
  mask_cfg.aspect_ratio_threshold = config.aspect_ratio_threshold;
  auto visual = features::compute_visual_analysis(frame, timestamp_ms, mask_cfg);

  const double fg_ratio = visual.signature.foreground_ratio;
  const std::uint64_t dhash = visual.signature.dhash;
  const bool has_sub = check_has_subtitle(fg_ratio, dhash, frame.width(), frame.height(), config);

  ImageBuffer roi_buf{};
  if (!config.pure_timing || config.retain_midpoint_in_pure_timing) {
    roi_buf = copy_view_to_buffer(frame);
  }

  return AnalyzedFrame{
      .frame_index = frame_index,
      .timestamp_ms = timestamp_ms,
      .dhash64 = dhash,
      .presence_ratio = static_cast<float>(fg_ratio),
      .has_subtitle = has_sub,
      .roi_image = std::move(roi_buf),
      .roi_mask = std::move(visual.valid_mask),
      .is_cut = false,
  };
}

AnalyzedFrame analyze_frame(
    ImageBuffer&& frame,
    const TimingConfig& config,
    std::uint64_t frame_index,
    std::int64_t timestamp_ms) {
  if (frame.empty()) {
    return AnalyzedFrame{
        .frame_index = frame_index,
        .timestamp_ms = timestamp_ms,
        .dhash64 = 0ULL,
        .presence_ratio = 0.0f,
        .has_subtitle = false,
        .roi_image = ImageBuffer{},
        .roi_mask = ImageBuffer{},
        .is_cut = false,
    };
  }

  const ImageView view = frame.view();
  features::MaskConfig mask_cfg{};
  mask_cfg.aspect_ratio_threshold = config.aspect_ratio_threshold;
  auto visual = features::compute_visual_analysis(view, timestamp_ms, mask_cfg);

  const double fg_ratio = visual.signature.foreground_ratio;
  const std::uint64_t dhash = visual.signature.dhash;
  const bool has_sub = check_has_subtitle(fg_ratio, dhash, view.width(), view.height(), config);

  ImageBuffer roi_buf{};
  if (!config.pure_timing || config.retain_midpoint_in_pure_timing) {
    roi_buf = std::move(frame);
  }

  return AnalyzedFrame{
      .frame_index = frame_index,
      .timestamp_ms = timestamp_ms,
      .dhash64 = dhash,
      .presence_ratio = static_cast<float>(fg_ratio),
      .has_subtitle = has_sub,
      .roi_image = std::move(roi_buf),
      .roi_mask = std::move(visual.valid_mask),
      .is_cut = false,
  };
}

} // namespace sublift::timing
