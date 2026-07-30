#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "sublift/models/model_bundle.hpp"

namespace sublift::models {

enum class ResourceSource {
  ExplicitOverride,
  AppBundle,
  UserCache,
  SystemPath
};

template <typename T>
struct ResourceResult {
  bool found{false};
  T value{};
  ResourceSource source{ResourceSource::UserCache};
  std::string error_msg{};
};

class ResourceLocator {
 public:
  ResourceLocator() = default;

  /// Probe and locate Paddle model bundle.
  [[nodiscard]] ResourceResult<ModelPaths> probe_model_bundle(
      const std::string& custom_dir = "", ModelType type = ModelType::Small) const;

  [[nodiscard]] ResourceResult<ModelPaths> locate_model_bundle(
      const std::string& custom_dir = "", ModelType type = ModelType::Small) const {
    return probe_model_bundle(custom_dir, type);
  }

  /// Locate FFmpeg / ffprobe executable.
  [[nodiscard]] ResourceResult<std::filesystem::path> locate_ffmpeg_executable(
      const std::string& custom_path = "") const;

  /// Locate ONNX Runtime shared library.
  [[nodiscard]] ResourceResult<std::filesystem::path> locate_onnxruntime_library(
      const std::string& custom_path = "") const;
};

}  // namespace sublift::models
