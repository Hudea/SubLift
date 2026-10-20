#include "path_media_services.hpp"

#include "sublift/adapters/ffmpeg.hpp"

namespace sublift::worker {
namespace {

class FfmpegStreamingExtractor final : public sublift::application::IStreamingExtractor {
 public:
  explicit FfmpegStreamingExtractor(double fps, std::optional<sublift::SourceBox> crop)
      : impl_(fps, std::move(crop)) {}

  void cancel() override { impl_.cancel(); }

  [[nodiscard]] std::optional<sublift::SourceBox> output_crop() const override {
    return impl_.output_crop();
  }

  void extract(const std::filesystem::path& video_path,
               const std::function<bool(sublift::Frame)>& consumer) override {
    impl_.extract(video_path, consumer);
  }

 private:
  sublift::ffmpeg::FfmpegExtractor impl_;
};

}  // namespace

sublift::FrameIOPlan FfmpegPathMediaServices::plan_frame_io(
    const std::filesystem::path& video_path,
    const std::optional<sublift::SourceBox>& region_box,
    std::string_view mode,
    double bottom_ratio) const {
  return sublift::ffmpeg::plan_frame_io(
      video_path, region_box, mode, sublift::ffmpeg::TransformPolicy::FallbackFull, bottom_ratio);
}

std::shared_ptr<sublift::application::IStreamingExtractor>
FfmpegPathMediaServices::create_extractor(
    double fps, const std::optional<sublift::SourceBox>& output_crop) const {
  return std::make_shared<FfmpegStreamingExtractor>(fps, output_crop);
}

std::int64_t FfmpegPathMediaServices::probe_duration_ms(
    const std::filesystem::path& video_path) const {
  return sublift::ffmpeg::probe_duration_ms(video_path);
}

sublift::application::FramePlanWithDuration FfmpegPathMediaServices::plan_frame_io_and_duration(
    const std::filesystem::path& video_path,
    const std::optional<sublift::SourceBox>& region_box,
    std::string_view mode,
    double bottom_ratio) const {
  // O7: one ffprobe for geometry + duration, then the pure planner. The
  // early pure branches (no region / mode=full) historically skipped the
  // source probe; the merged probe is still a single call in those cases
  // (the duration probe), so the spawn count never increases.
  const auto probed = sublift::ffmpeg::probe_source_and_duration(video_path);
  return sublift::application::FramePlanWithDuration{
      .plan = sublift::ffmpeg::plan_frame_io_pure(
          region_box, mode, probed.source, sublift::ffmpeg::TransformPolicy::FallbackFull,
          bottom_ratio),
      .duration_ms = probed.duration_ms,
  };
}

}  // namespace sublift::worker
