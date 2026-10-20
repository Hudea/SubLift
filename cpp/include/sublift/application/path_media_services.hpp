#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string_view>

#include "sublift/extractor.hpp"
#include "sublift/models.hpp"

namespace sublift::application {

/// Streaming extractor used by path-mode jobs (cancel + frame callback).
class IStreamingExtractor : public sublift::IExtractor {
 public:
  ~IStreamingExtractor() override = default;

  virtual void extract(const std::filesystem::path& video_path,
                       const std::function<bool(sublift::Frame)>& consumer) = 0;
};

/// Frame I/O plan plus the job's duration, from a single media probe (O7).
struct FramePlanWithDuration {
  sublift::FrameIOPlan plan;
  std::int64_t duration_ms{0};
};

/// Composition-root services for path-mode media (plan / probe / extractor).
/// Application must not construct concrete FFmpeg adapters directly.
class IPathMediaServices {
 public:
  virtual ~IPathMediaServices() = default;

  [[nodiscard]] virtual sublift::FrameIOPlan plan_frame_io(
      const std::filesystem::path& video_path,
      const std::optional<sublift::SourceBox>& region_box,
      std::string_view mode,
      double bottom_ratio) const = 0;

  /// Plan + duration in one probe (O7). The default implementation keeps the
  /// historical two-probe behavior; the FFmpeg composition root overrides it
  /// with a single merged ffprobe call.
  [[nodiscard]] virtual FramePlanWithDuration plan_frame_io_and_duration(
      const std::filesystem::path& video_path,
      const std::optional<sublift::SourceBox>& region_box,
      std::string_view mode,
      double bottom_ratio) const {
    return FramePlanWithDuration{
        .plan = plan_frame_io(video_path, region_box, mode, bottom_ratio),
        .duration_ms = probe_duration_ms(video_path),
    };
  }

  [[nodiscard]] virtual std::shared_ptr<IStreamingExtractor> create_extractor(
      double fps, const std::optional<sublift::SourceBox>& output_crop) const = 0;

  [[nodiscard]] virtual std::int64_t probe_duration_ms(
      const std::filesystem::path& video_path) const = 0;
};

}  // namespace sublift::application
