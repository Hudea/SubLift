#pragma once

#include "sublift/application/path_media_services.hpp"

namespace sublift::worker {

/// Worker-side FFmpeg-backed path media services (Composition Root).
class FfmpegPathMediaServices final : public sublift::application::IPathMediaServices {
 public:
  FfmpegPathMediaServices() = default;

  [[nodiscard]] sublift::FrameIOPlan plan_frame_io(
      const std::filesystem::path& video_path,
      const std::optional<sublift::SourceBox>& region_box,
      std::string_view mode,
      double bottom_ratio) const override;

  [[nodiscard]] std::shared_ptr<sublift::application::IStreamingExtractor> create_extractor(
      double fps, const std::optional<sublift::SourceBox>& output_crop) const override;

  [[nodiscard]] std::int64_t probe_duration_ms(
      const std::filesystem::path& video_path) const override;
};

}  // namespace sublift::worker
