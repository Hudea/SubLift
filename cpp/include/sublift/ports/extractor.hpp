#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>

#include "sublift/detector.hpp"
#include "sublift/models.hpp"

namespace sublift {

/// source-frame 探测结果（与 Python SourceFrameInfo 语义与字段 1:1 对齐）
struct SourceFrameInfo {
  std::int32_t width{0};
  std::int32_t height{0};
  bool display_transform_ok{true};
  std::optional<std::string> transform_note;

  friend bool operator==(const SourceFrameInfo&, const SourceFrameInfo&) = default;
};

/// 视频元数据（与 Python VideoInfo 对齐）
struct VideoInfo {
  std::int32_t width{0};
  std::int32_t height{0};
  std::int64_t duration_ms{0};

  friend bool operator==(const VideoInfo&, const VideoInfo&) = default;
};

/// 帧输出模式枚举（与 Python FrameOutputMode 对齐）
enum class FrameOutputMode {
  FullRgb,  // full_rgb
  RoiRgb    // roi_rgb
};

/// 帧 IO 规划结果（与 Python FrameIOPlan 几何/模式与 detector 组成部分对齐）
struct FrameIOPlan {
  std::optional<SourceBox> output_crop;
  FrameOutputMode output_mode{FrameOutputMode::FullRgb};
  std::optional<SourceFrameInfo> source;
  std::optional<std::string> fallback_reason;
  std::shared_ptr<IDetector> detector;

  [[nodiscard]] bool operator==(const FrameIOPlan& other) const noexcept {
    if (output_crop != other.output_crop ||
        output_mode != other.output_mode ||
        source != other.source ||
        fallback_reason != other.fallback_reason) {
      return false;
    }
    if (detector == other.detector) {
      return true;
    }
    if (!detector || !other.detector) {
      return false;
    }
    return detector->is_equal(*other.detector);
  }
};

/// Extractor 抽象接口
class IExtractor {
public:
  virtual ~IExtractor() = default;

  /// 强制/协作式取消抽帧
  virtual void cancel() = 0;

  /// 查询当前配置的 source-frame crop 区域（std::nullopt 表示全帧）
  [[nodiscard]] virtual std::optional<SourceBox> output_crop() const = 0;
};

}  // namespace sublift
