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

  [[nodiscard]] virtual std::shared_ptr<IStreamingExtractor> create_extractor(
      double fps, const std::optional<sublift::SourceBox>& output_crop) const = 0;

  [[nodiscard]] virtual std::int64_t probe_duration_ms(
      const std::filesystem::path& video_path) const = 0;
};

}  // namespace sublift::application
