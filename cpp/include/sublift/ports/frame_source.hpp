#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include <sublift/core/box.hpp>
#include <sublift/core/frame.hpp>
#include <sublift/core/image_buffer.hpp>

namespace sublift {

struct VideoInfo {
  std::int32_t width{0};
  std::int32_t height{0};
  std::int64_t duration_ms{0};
  double fps{0.0};
  bool display_transform_ok{true};
  std::optional<std::string> transform_note{std::nullopt};
  friend bool operator==(const VideoInfo&, const VideoInfo&) = default;
};

struct SourceFrameInfo {
  std::int32_t width{0};
  std::int32_t height{0};
  bool display_transform_ok{true};
  std::optional<std::string> transform_note;
  std::int32_t pixel_aspect_num{1};
  std::int32_t pixel_aspect_den{1};
  std::int64_t relative_video_start_ms{0};
  friend bool operator==(const SourceFrameInfo&, const SourceFrameInfo&) = default;
};

enum class FrameOutputMode {
  FullRgb,
  RoiRgb
};

// 纯数据决策计划：FrameIOPlan 不持有 IDetector 指针，消除职责倒置
struct FrameIOPlan {
  std::optional<SourceBox> output_crop;
  FrameOutputMode output_mode{FrameOutputMode::FullRgb};
  std::optional<SourceFrameInfo> source;
  std::optional<std::string> fallback_reason;
  friend bool operator==(const FrameIOPlan&, const FrameIOPlan&) = default;
};

struct KeyframeThumbnail {
  std::int64_t timestamp_ms{0};
  ImageBuffer image{};  // 320x80 Grayscale (PixelFormat::Gray8)

  [[nodiscard]] KeyframeThumbnail clone() const {
    return KeyframeThumbnail{
        .timestamp_ms = timestamp_ms,
        .image = image.clone(),
    };
  }

  bool operator==(const KeyframeThumbnail& other) const noexcept {
    if (timestamp_ms != other.timestamp_ms) {
      return false;
    }
    if (image.width() != other.image.width() ||
        image.height() != other.image.height() ||
        image.format() != other.image.format()) {
      return false;
    }
    if (image.empty() && other.image.empty()) {
      return true;
    }
    if (image.empty() != other.image.empty()) {
      return false;
    }
    const std::size_t size = static_cast<std::size_t>(image.height()) *
                             static_cast<std::size_t>(image.stride_bytes());
    return std::memcmp(image.data(), other.image.data(), size) == 0;
  }
};

class IFrameSource {
 public:
  virtual ~IFrameSource() = default;

  [[nodiscard]] virtual VideoInfo probe(const std::filesystem::path& video_path) = 0;

  [[nodiscard]] virtual FrameIOPlan plan(
      const VideoInfo& info,
      const std::optional<SourceBox>& requested_box) = 0;

  virtual void extract(
      const std::filesystem::path& video_path,
      double fps,
      const std::optional<SourceBox>& crop,
      std::function<bool(Frame)> consumer) = 0;

  [[nodiscard]] virtual ImageBuffer extract_frame(
      const std::filesystem::path& video_path,
      std::int64_t timestamp_ms,
      const std::optional<SourceBox>& crop) = 0;

  /// 轻量级关键帧缩略图流式采样契约（提供默认空实现保障向后兼容）
  [[nodiscard]] virtual std::vector<KeyframeThumbnail> sample_keyframe_thumbnails(
      const std::filesystem::path& video_path,
      double bottom_ratio = 0.45,
      std::size_t max_samples = 60) {
    (void)video_path;
    (void)bottom_ratio;
    (void)max_samples;
    return {};
  }

  virtual void cancel() = 0;
  [[nodiscard]] virtual bool is_cancelled() const noexcept = 0;
};

} // namespace sublift
