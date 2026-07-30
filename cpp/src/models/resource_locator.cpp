#include "sublift/models/resource_locator.hpp"

#include <cstdlib>
#include <sstream>
#include <unistd.h>

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

  // Support both product layout locations:
  //   SubLift.app/Contents/MacOS/<cli>
  //   SubLift.app/Contents/Helpers/sublift_worker
  auto dir = exe->parent_path();
  const auto parent_name = dir.filename();
  if ((parent_name == "MacOS" || parent_name == "Helpers") &&
      dir.parent_path().filename() == "Contents") {
    auto contents = dir.parent_path();
    auto candidate = contents / subpath;
    if (std::filesystem::exists(candidate)) {
      return candidate;
    }
  }
  return std::nullopt;
}

bool is_executable_file(const std::filesystem::path& p) {
  std::error_code ec;
  if (!std::filesystem::exists(p, ec) || !std::filesystem::is_regular_file(p, ec)) {
    return false;
  }
#if defined(_WIN32)
  return true;
#else
  return ::access(p.c_str(), X_OK) == 0;
#endif
}

std::optional<std::filesystem::path> find_on_path(const std::string& name) {
  const char* path_env = std::getenv("PATH");
  if (path_env == nullptr || *path_env == '\0') return std::nullopt;
  std::string env_str{path_env};
  std::stringstream ss{env_str};
  std::string dir;
  while (std::getline(ss, dir, ':')) {
    if (dir.empty()) continue;
    auto candidate = std::filesystem::path{dir} / name;
    if (is_executable_file(candidate)) return candidate;
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

ModelPaths ResourceLocator::locate_model_bundle(
    const std::string& custom_dir, ModelType type) const {
  auto res = probe_model_bundle(custom_dir, type);
  if (!res.found) {
    throw std::runtime_error("Failed to locate model bundle: " + res.error_msg);
  }
  return res.value;
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
    if (is_executable_file(*app_bin)) {
      res.found = true;
      res.value = *app_bin;
      res.source = ResourceSource::AppBundle;
      return res;
    }
  }

  // Prefer PATH (includes Homebrew when configured), then common absolute fallbacks.
  if (auto on_path = find_on_path("ffmpeg"); on_path.has_value()) {
    res.found = true;
    res.value = *on_path;
    res.source = ResourceSource::SystemPath;
    return res;
  }

  const std::vector<std::filesystem::path> candidates = {
      "/opt/homebrew/opt/ffmpeg-full/bin/ffmpeg",
      "/opt/homebrew/bin/ffmpeg",
      "/usr/local/bin/ffmpeg",
      "/usr/bin/ffmpeg",
  };
  for (const auto& c : candidates) {
    if (is_executable_file(c)) {
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

ResourceResult<std::filesystem::path> ResourceLocator::locate_ffprobe_executable(
    const std::string& custom_path) const {
  ResourceResult<std::filesystem::path> res;
  if (!custom_path.empty()) {
    auto p = expand_user_path(custom_path);
    res.source = ResourceSource::ExplicitOverride;
    if (is_executable_file(p)) {
      res.found = true;
      res.value = p;
    } else {
      res.found = false;
      res.error_msg = "ffprobe executable not found at custom path: " + p.string();
    }
    return res;
  }

  if (const char* env = std::getenv("SUBLIFT_FFPROBE_PATH"); env != nullptr && *env != '\0') {
    auto p = expand_user_path(env);
    res.source = ResourceSource::ExplicitOverride;
    if (is_executable_file(p)) {
      res.found = true;
      res.value = p;
    } else {
      res.found = false;
      res.error_msg = "ffprobe executable not found at SUBLIFT_FFPROBE_PATH: " + p.string();
    }
    return res;
  }

  if (auto app_bin = find_app_bundle_resource("Resources/bin/ffprobe"); app_bin.has_value()) {
    if (is_executable_file(*app_bin)) {
      res.found = true;
      res.value = *app_bin;
      res.source = ResourceSource::AppBundle;
      return res;
    }
  }

  // Sibling of bundled ffmpeg often shares the same bin dir.
  if (auto ffmpeg = locate_ffmpeg_executable(""); ffmpeg.found) {
    auto sibling = ffmpeg.value.parent_path() / "ffprobe";
    if (is_executable_file(sibling)) {
      res.found = true;
      res.value = sibling;
      res.source = ffmpeg.source;
      return res;
    }
  }

  if (auto on_path = find_on_path("ffprobe"); on_path.has_value()) {
    res.found = true;
    res.value = *on_path;
    res.source = ResourceSource::SystemPath;
    return res;
  }

  const std::vector<std::filesystem::path> candidates = {
      "/opt/homebrew/opt/ffmpeg-full/bin/ffprobe",
      "/opt/homebrew/bin/ffprobe",
      "/usr/local/bin/ffprobe",
      "/usr/bin/ffprobe",
  };
  for (const auto& c : candidates) {
    if (is_executable_file(c)) {
      res.found = true;
      res.value = c;
      res.source = ResourceSource::SystemPath;
      return res;
    }
  }

  res.found = false;
  res.error_msg = "ffprobe executable not found";
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
