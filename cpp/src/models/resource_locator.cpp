#include "sublift/models/resource_locator.hpp"

#include <cstdlib>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace sublift::models {

namespace {

std::optional<std::filesystem::path> current_executable_path() {
#if defined(__APPLE__)
  char buf[4096];
  uint32_t size = sizeof(buf);
  if (_NSGetExecutablePath(buf, &size) == 0) {
    std::error_code ec;
    auto canonical = std::filesystem::weakly_canonical(std::filesystem::path{buf}, ec);
    if (!ec) return canonical;
    return std::filesystem::path{buf};
  }
#else
  std::error_code ec;
  auto self = std::filesystem::read_symlink("/proc/self/exe", ec);
  if (!ec) return self;
#endif
  return std::nullopt;
}

std::optional<std::filesystem::path> find_app_bundle_resource(const std::string& subpath) {
  auto exe = current_executable_path();
  if (!exe.has_value()) return std::nullopt;

  // Check if inside macOS .app Bundle: <app>.app/Contents/MacOS/<exe>
  auto dir = exe->parent_path();
  if (dir.filename() == "MacOS" && dir.parent_path().filename() == "Contents") {
    auto contents = dir.parent_path();
    auto candidate = contents / subpath;
    if (std::filesystem::exists(candidate)) {
      return candidate;
    }
  }
  return std::nullopt;
}

}  // namespace

ResourceResult<ModelPaths> ResourceLocator::probe_model_bundle(
    const std::string& custom_dir, ModelType type) const {
  ResourceResult<ModelPaths> res;

  std::filesystem::path target_dir;
  ResourceSource src = ResourceSource::UserCache;

  if (!custom_dir.empty()) {
    target_dir = expand_user_path(custom_dir);
    src = ResourceSource::ExplicitOverride;
  } else if (const char* env_dir = std::getenv("SUBLIFT_PADDLE_MODEL_DIR");
             env_dir != nullptr && *env_dir != '\0') {
    target_dir = expand_user_path(env_dir);
    src = ResourceSource::ExplicitOverride;
  } else if (auto app_models = find_app_bundle_resource("Resources/models"); app_models.has_value()) {
    target_dir = *app_models;
    src = ResourceSource::AppBundle;
  } else {
    target_dir = resolve_model_dir("");
    src = ResourceSource::UserCache;
  }

  res.source = src;
  res.value = get_expected_model_paths(target_dir, type);
  res.found = validate_model_paths(res.value, &res.error_msg);
  return res;
}

ResourceResult<std::filesystem::path> ResourceLocator::locate_ffmpeg_executable(
    const std::string& custom_path) const {
  ResourceResult<std::filesystem::path> res;
  if (!custom_path.empty()) {
    auto p = expand_user_path(custom_path);
    res.source = ResourceSource::ExplicitOverride;
    if (std::filesystem::exists(p)) {
      res.found = true;
      res.value = p;
    } else {
      res.found = false;
      res.error_msg = "FFmpeg executable not found at custom path: " + p.string();
    }
    return res;
  }

  if (const char* env = std::getenv("SUBLIFT_FFMPEG_PATH"); env != nullptr && *env != '\0') {
    auto p = expand_user_path(env);
    res.source = ResourceSource::ExplicitOverride;
    if (std::filesystem::exists(p)) {
      res.found = true;
      res.value = p;
    } else {
      res.found = false;
      res.error_msg = "FFmpeg executable not found at SUBLIFT_FFMPEG_PATH: " + p.string();
    }
    return res;
  }

  if (auto app_bin = find_app_bundle_resource("Resources/bin/ffmpeg"); app_bin.has_value()) {
    res.found = true;
    res.value = *app_bin;
    res.source = ResourceSource::AppBundle;
    return res;
  }

  // System PATH / common Homebrew
  const std::vector<std::filesystem::path> candidates = {
      "/opt/homebrew/bin/ffmpeg",
      "/usr/local/bin/ffmpeg",
      "/usr/bin/ffmpeg",
  };
  for (const auto& c : candidates) {
    if (std::filesystem::exists(c)) {
      res.found = true;
      res.value = c;
      res.source = ResourceSource::SystemPath;
      return res;
    }
  }

  res.found = false;
  res.error_msg = "FFmpeg executable not found";
  return res;
}

ResourceResult<std::filesystem::path> ResourceLocator::locate_onnxruntime_library(
    const std::string& custom_path) const {
  ResourceResult<std::filesystem::path> res;
  if (!custom_path.empty()) {
    auto p = expand_user_path(custom_path);
    res.source = ResourceSource::ExplicitOverride;
    if (std::filesystem::exists(p)) {
      res.found = true;
      res.value = p;
    } else {
      res.found = false;
      res.error_msg = "ONNX Runtime library not found at custom path: " + p.string();
    }
    return res;
  }

  if (const char* env = std::getenv("SUBLIFT_ORT_LIB_DIR"); env != nullptr && *env != '\0') {
    auto p = expand_user_path(env);
    res.source = ResourceSource::ExplicitOverride;
    if (std::filesystem::exists(p)) {
      res.found = true;
      res.value = p;
    } else {
      res.found = false;
      res.error_msg = "ONNX Runtime library not found at SUBLIFT_ORT_LIB_DIR: " + p.string();
    }
    return res;
  }

  if (auto app_fw = find_app_bundle_resource("Frameworks/libonnxruntime.dylib"); app_fw.has_value()) {
    res.found = true;
    res.value = *app_fw;
    res.source = ResourceSource::AppBundle;
    return res;
  }

  res.found = false;
  res.error_msg = "ONNX Runtime library not found";
  return res;
}

}  // namespace sublift::models
