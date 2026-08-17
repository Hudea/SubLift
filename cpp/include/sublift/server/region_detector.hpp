#pragma once

#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "sublift/models.hpp"
#include "sublift/ocr.hpp"

namespace sublift::server {

struct NormalizedBox {
  double x{0.0};
  double y{0.70};
  double width{1.0};
  double height{0.25};
};

struct RegionDetectionResult {
  bool detected{false};
  double sample_time_s{0.0};
  NormalizedBox suggested_box;
  std::string preview_text;
  double confidence{0.0};
  int total_candidates{0};
  std::string error_msg;
};

class RegionDetector {
 public:
  explicit RegionDetector(std::shared_ptr<IOcrEngine> shared_ocr = nullptr);
  ~RegionDetector() = default;

  /// 执行自动字幕区域探测
  /// @param video_path 视频物理路径（已通过 sandbox 校验）
  /// @param playhead_time_s 若提供，则为单帧即时重检模式；若 nullopt 则为全视频多点采样初识模式
  /// @param engine_preference 指定引擎 ("vision" / "paddle")
  [[nodiscard]] RegionDetectionResult detect_region(
      const std::filesystem::path& video_path,
      std::optional<double> playhead_time_s = std::nullopt,
      const std::optional<std::string>& engine_preference = std::nullopt);

 private:
  IOcrEngine* resolve_ocr_engine_locked(const std::optional<std::string>& engine_preference);

  std::shared_ptr<IOcrEngine> shared_ocr_;
  std::unique_ptr<IOcrEngine> fallback_engine_;
  std::string current_engine_name_;
  std::mutex ocr_mutex_;
};

}  // namespace sublift::server
