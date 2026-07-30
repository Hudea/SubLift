#include "path_media_services.hpp"

#include "sublift/ffmpeg.hpp"

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

}  // namespace sublift::worker
