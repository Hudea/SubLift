#include "sublift/models/resource_locator.hpp"

#include "sublift/hash.hpp"
#include "sublift/models/native_manifest.hpp"

#include <cstdlib>
#include <functional>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <unistd.h>
#include <vector>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace sublift::models {

namespace {

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

std::optional<std::filesystem::path> process_executable_dir() {
#if defined(__APPLE__)
  char buf[4096];
  uint32_t size = sizeof(buf);
  if (_NSGetExecutablePath(buf, &size) != 0) return std::nullopt;
  std::error_code ec;
  auto p = std::filesystem::weakly_canonical(std::filesystem::path{buf}, ec);
  if (ec) p = std::filesystem::path{buf};
  return p.parent_path();
#else
  std::error_code ec;
  auto self = std::filesystem::read_symlink("/proc/self/exe", ec);
  if (ec) return std::nullopt;
  return self.parent_path();
#endif
}

bool digest_matches_manifest(const std::filesystem::path& dir,
                             const PaddleBundleSpec& spec, std::string* error) {
  std::vector<std::string> bad;
  for (const auto& file : spec.files) {
    const auto path = dir / file.filename;
    auto digest = sha256_file_hex(path);
    if (!digest || *digest != file.sha256) {
      bad.push_back(file.filename);
    }
  }
  if (bad.empty()) return true;
  if (error) {
    *error = "Paddle 模型 SHA-256 不匹配（不是可用资源）: ";
    for (size_t i = 0; i < bad.size(); ++i) {
      if (i > 0) *error += ", ";
      *error += bad[i];
    }
  }
  return false;
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
    const std::string& custom_dir, ModelType type, bool require_digest) const {
  ResourceResult<ModelPaths> res;

  struct Candidate {
    std::filesystem::path dir;
    ResourceSource source;
  };
  std::vector<Candidate> candidates;

  if (!custom_dir.empty()) {
    candidates.push_back({expand_user_path(custom_dir), ResourceSource::ExplicitOverride});
  } else if (const char* env_dir = std::getenv("SUBLIFT_PADDLE_MODEL_DIR");
             env_dir != nullptr && *env_dir != '\0') {
    candidates.push_back({expand_user_path(env_dir), ResourceSource::ExplicitOverride});
  } else {
    if (auto exe = process_executable_dir()) {
      candidates.push_back({*exe / "models", ResourceSource::Bundled});
      candidates.push_back({exe->parent_path() / "share" / "sublift" / "models",
                            ResourceSource::Bundled});
      if (exe->filename() == "MacOS" && exe->parent_path().filename() == "Contents") {
        candidates.push_back(
            {exe->parent_path() / "Resources" / "models", ResourceSource::Bundled});
      }
    }
    candidates.push_back({std::filesystem::path{"/opt/sublift/models"}, ResourceSource::Bundled});
    candidates.push_back({default_native_model_dir(), ResourceSource::UserCache});
    candidates.push_back({default_legacy_model_dir(), ResourceSource::UserCache});
  }

  ManifestLoadResult manifest;
  if (require_digest) {
    if (type != ModelType::Small) {
      res.error_msg =
          "Paddle model type '" + model_type_to_string(type) +
          "' is not pinned in resources/manifest.json";
      return res;
    }
    manifest = load_default_native_manifest();
    if (!manifest.ok) {
      res.error_msg = manifest.error_msg;
      return res;
    }
  }

  std::string last_error = "Paddle 模型目录不存在";
  for (const auto& cand : candidates) {
    res.source = cand.source;
    res.value = get_expected_model_paths(cand.dir, type);
    std::string exists_error;
    if (!validate_model_paths(res.value, &exists_error)) {
      last_error = exists_error;
      continue;
    }
    if (require_digest) {
      std::string digest_error;
      if (!digest_matches_manifest(cand.dir, manifest.manifest.paddle, &digest_error)) {
        last_error = digest_error;
        continue;
      }
    }
    res.found = true;
    res.error_msg.clear();
    return res;
  }

  res.found = false;
  res.error_msg = last_error;
  return res;
}

ModelPaths ResourceLocator::locate_model_bundle(
    const std::string& custom_dir, ModelType type, bool require_digest) const {
  auto res = probe_model_bundle(custom_dir, type, require_digest);
  if (!res.found) {
    throw std::runtime_error(res.error_msg);
  }
  return res.value;
}

ResourceResult<std::filesystem::path> ResourceLocator::locate_ffmpeg_executable(
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
      res.error_msg = "FFmpeg executable not found at custom path: " + p.string();
    }
    return res;
  }

  if (const char* env = std::getenv("SUBLIFT_FFMPEG_PATH"); env != nullptr && *env != '\0') {
    auto p = expand_user_path(env);
    res.source = ResourceSource::ExplicitOverride;
    if (is_executable_file(p)) {
      res.found = true;
      res.value = p;
    } else {
      res.found = false;
      res.error_msg = "FFmpeg executable not found at SUBLIFT_FFMPEG_PATH: " + p.string();
    }
    return res;
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

  // ffprobe commonly shares the same bin directory as ffmpeg.
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
  std::function<bool(const std::filesystem::path&, ResourceSource)> consider;
  consider = [&](const std::filesystem::path& raw, ResourceSource source) -> bool {
    auto p = expand_user_path(raw);
    if (is_python_packaged_path(p)) {
      res.source = source;
      res.found = false;
      res.error_msg =
          "ONNX Runtime library is a Python wheel path and is not a product source: " +
          p.string();
      return false;
    }
    if (std::filesystem::is_directory(p)) {
      const std::vector<std::filesystem::path> names = {
          p / "libonnxruntime.1.28.0.dylib",
          p / "libonnxruntime.1.dylib",
          p / "libonnxruntime.dylib",
          p / "libonnxruntime.so.1.18.1",
          p / "libonnxruntime.so.1",
          p / "libonnxruntime.so",
      };
      for (const auto& n : names) {
        if (consider(n, source)) return true;
      }
      return false;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(p, ec)) return false;
    auto manifest = load_default_native_manifest();
    if (manifest.ok) {
      std::string digest_error;
      if (!ort_library_digest_allowed(manifest.manifest, p, &digest_error)) {
        res.source = source;
        res.found = false;
        res.error_msg = digest_error;
        return false;
      }
    }
    res.found = true;
    res.value = p;
    res.source = source;
    res.error_msg.clear();
    return true;
  };

  if (!custom_path.empty()) {
    if (consider(custom_path, ResourceSource::ExplicitOverride)) return res;
    if (res.error_msg.empty()) {
      res.source = ResourceSource::ExplicitOverride;
      res.error_msg = "ONNX Runtime library not found at custom path: " + custom_path;
    }
    return res;
  }

  if (const char* env = std::getenv("SUBLIFT_ORT_LIB_DIR"); env != nullptr && *env != '\0') {
    if (consider(env, ResourceSource::ExplicitOverride)) return res;
    if (res.error_msg.empty()) {
      res.source = ResourceSource::ExplicitOverride;
      res.error_msg = std::string("ONNX Runtime library not found at SUBLIFT_ORT_LIB_DIR: ") + env;
    }
    return res;
  }

  if (auto exe = process_executable_dir()) {
    if (consider(*exe / ".." / "lib", ResourceSource::Bundled)) return res;
    if (consider(*exe / "lib", ResourceSource::Bundled)) return res;
  }
  if (consider(default_ort_cache_dir(), ResourceSource::UserCache)) return res;

  const std::vector<std::filesystem::path> system_dirs = {
      "/opt/homebrew/opt/onnxruntime/lib",
      "/opt/homebrew/lib",
      "/usr/local/lib",
      "/usr/lib",
  };
  for (const auto& dir : system_dirs) {
    if (consider(dir, ResourceSource::SystemPath)) return res;
  }

  if (res.error_msg.empty()) {
    res.error_msg = "ONNX Runtime library not found";
  }
  res.found = false;
  return res;
}

}  // namespace sublift::models
