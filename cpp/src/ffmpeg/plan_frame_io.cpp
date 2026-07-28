#include "sublift/ffmpeg.hpp"

#include <memory>
#include <stdexcept>
#include <string>

#include "sublift/bottom_crop_detector.hpp"
#include "sublift/fixed_detector.hpp"
#include "sublift/roi_passthrough_detector.hpp"

namespace sublift::ffmpeg {

FrameIOPlan plan_frame_io_pure(
    const std::optional<SourceBox>& region_box,
    std::string_view mode,
    const std::optional<SourceFrameInfo>& source,
    TransformPolicy on_unvalidated_transform,
    double bottom_ratio) {

  if (mode != "auto" && mode != "full" && mode != "roi") {
    throw std::invalid_argument("mode 必须是 auto|full|roi（收到 '" + std::string(mode) + "'）");
  }
  if (mode == "roi" && !region_box.has_value()) {
    throw std::invalid_argument("mode=roi 需要 region_box");
  }

  if (!region_box.has_value()) {
    return FrameIOPlan{
        .output_crop = std::nullopt,
        .output_mode = FrameOutputMode::FullRgb,
        .source = std::nullopt,
        .fallback_reason = std::nullopt,
        .detector = std::make_shared<BottomCropDetector>(bottom_ratio),
    };
  }

  if (mode == "full") {
    FrameLocalBox fixed_box{region_box->x, region_box->y, region_box->width, region_box->height};
    return FrameIOPlan{
        .output_crop = std::nullopt,
        .output_mode = FrameOutputMode::FullRgb,
        .source = std::nullopt,
        .fallback_reason = std::nullopt,
        .detector = std::make_shared<FixedRegionDetector>(fixed_box),
    };
  }

  // auto 或 roi：需要 source
  if (!source.has_value()) {
    throw std::invalid_argument("plan_frame_io_pure: mode=auto|roi 且有 region 时需要 source");
  }

  const SourceFrameInfo& src = *source;
  const bool want_roi = (mode == "auto" || mode == "roi");

  if (want_roi && !src.display_transform_ok) {
    if (mode == "roi" || on_unvalidated_transform == TransformPolicy::Error) {
      std::string note_str = src.transform_note.has_value() ? "'" + *src.transform_note + "'" : "None";
      throw std::runtime_error("ROI 输出要求已验证的恒等显示变换，当前未验证: note=" +
                               note_str + " source=" + std::to_string(src.width) + "x" +
                               std::to_string(src.height));
    }
    std::string fallback = (src.transform_note.has_value() && !src.transform_note->empty())
                               ? *src.transform_note
                               : "unvalidated_display_transform";
    FrameLocalBox fixed_box{region_box->x, region_box->y, region_box->width, region_box->height};
    return FrameIOPlan{
        .output_crop = std::nullopt,
        .output_mode = FrameOutputMode::FullRgb,
        .source = src,
        .fallback_reason = fallback,
        .detector = std::make_shared<FixedRegionDetector>(fixed_box),
    };
  }

  validate_output_crop(*region_box, src.width, src.height);

  return FrameIOPlan{
      .output_crop = region_box,
      .output_mode = FrameOutputMode::RoiRgb,
      .source = src,
      .fallback_reason = std::nullopt,
      .detector = std::make_shared<RoiPassthroughDetector>(region_box->width, region_box->height),
  };
}

FrameIOPlan plan_frame_io(
    const std::filesystem::path& video_path,
    const std::optional<SourceBox>& region_box,
    std::string_view mode,
    TransformPolicy on_unvalidated_transform,
    double bottom_ratio) {

  // Early pure branches that do not need probe (same as Python).
  if (mode != "auto" && mode != "full" && mode != "roi") {
    throw std::invalid_argument("mode 必须是 auto|full|roi（收到 '" + std::string(mode) + "'）");
  }
  if (mode == "roi" && !region_box.has_value()) {
    throw std::invalid_argument("mode=roi 需要 region_box");
  }
  if (!region_box.has_value() || mode == "full") {
    return plan_frame_io_pure(region_box, mode, std::nullopt, on_unvalidated_transform,
                              bottom_ratio);
  }

  const SourceFrameInfo source = probe_source_frame(video_path);
  return plan_frame_io_pure(region_box, mode, source, on_unvalidated_transform, bottom_ratio);
}

}  // namespace sublift::ffmpeg
