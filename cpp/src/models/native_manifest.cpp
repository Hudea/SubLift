#include "sublift/models/native_manifest.hpp"

#include "sublift/models/model_bundle.hpp"

#include <cstdlib>
#include <fstream>
#include <iterator>

#include <nlohmann/json.hpp>

#include "sublift/hash.hpp"

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace sublift::models {
namespace {

std::string read_file_text(const std::filesystem::path& path, std::string* error) {
  std::ifstream in(path);
  if (!in) {
    *error = "cannot read manifest: " + path.string();
    return {};
  }
  return std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
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

}  // namespace

bool is_python_packaged_path(const std::filesystem::path& path) {
  const auto s = path.generic_string();
  return s.find("/.venv/") != std::string::npos ||
         s.find("site-packages") != std::string::npos ||
         s.find("/onnxruntime/capi/") != std::string::npos;
}

ManifestLoadResult load_native_manifest(const std::filesystem::path& path) {
  ManifestLoadResult out;
  out.path = path;
  std::string error;
  const auto text = read_file_text(path, &error);
  if (text.empty() && !error.empty()) {
    out.error_msg = error;
    return out;
  }
  try {
    const auto json = nlohmann::json::parse(text);
    if (!json.is_object()) {
      out.error_msg = "manifest root must be an object";
      return out;
    }
    out.manifest.schema_version = json.value("schema_version", 0);
    out.manifest.kind = json.value("kind", std::string{});
    if (out.manifest.schema_version != 1 || out.manifest.kind != "native-resources") {
      out.error_msg = "unsupported native resource manifest";
      return out;
    }
    const auto& paddle = json.at("paddle");
    out.manifest.paddle.id = paddle.value("id", std::string{"ppocrv6-small"});
    out.manifest.paddle.version = paddle.value("version", std::string{});
    out.manifest.paddle.install_subdir =
        paddle.value("install_subdir", std::string{"models/ppocrv6-small"});
    for (const auto& file : paddle.at("files")) {
      ResourceFileSpec spec;
      spec.role = file.value("role", std::string{});
      spec.filename = file.at("filename").get<std::string>();
      spec.sha256 = file.at("sha256").get<std::string>();
      spec.url = file.value("url", std::string{});
      if (spec.filename.empty() || spec.sha256.size() != 64) {
        out.error_msg = "paddle file spec missing filename or sha256";
        return out;
      }
      out.manifest.paddle.files.push_back(std::move(spec));
    }
    if (out.manifest.paddle.files.empty()) {
      out.error_msg = "paddle files list is empty";
      return out;
    }
    if (json.contains("onnxruntime")) {
      for (const auto& art : json.at("onnxruntime")) {
        OrtArtifactSpec spec;
        spec.os = art.value("os", std::string{});
        spec.arch = art.value("arch", std::string{});
        spec.version = art.value("version", std::string{});
        spec.library = art.value("library", std::string{});
        spec.sha256 = art.value("sha256", std::string{});
        spec.url = art.value("url", std::string{});
        spec.tarball_sha256 = art.value("tarball_sha256", std::string{});
        spec.source = art.value("source", std::string{});
        out.manifest.onnxruntime.push_back(std::move(spec));
      }
    }
    out.ok = true;
    return out;
  } catch (const std::exception& ex) {
    out.error_msg = std::string("manifest parse error: ") + ex.what();
    return out;
  }
}

ManifestLoadResult load_default_native_manifest(
    const std::optional<std::filesystem::path>& executable_dir) {
  std::vector<std::filesystem::path> candidates;
  if (const char* env = std::getenv("SUBLIFT_NATIVE_MANIFEST");
      env != nullptr && *env != '\0') {
    candidates.emplace_back(env);
  }
#ifdef SUBLIFT_NATIVE_MANIFEST_PATH
  candidates.emplace_back(SUBLIFT_NATIVE_MANIFEST_PATH);
#endif
  const auto exe_dir = executable_dir.has_value() ? executable_dir : process_executable_dir();
  if (exe_dir.has_value()) {
    candidates.push_back(*exe_dir / "share" / "sublift" / "manifest.v1.json");
    candidates.push_back(exe_dir->parent_path() / "share" / "sublift" / "manifest.v1.json");
  }
  std::error_code ec;
  const auto cwd = std::filesystem::current_path(ec);
  if (!ec) {
    candidates.push_back(cwd / "native-resources" / "manifest.v1.json");
    candidates.push_back(cwd.parent_path() / "native-resources" / "manifest.v1.json");
  }

  ManifestLoadResult last;
  last.error_msg = "native resource manifest not found";
  for (const auto& c : candidates) {
    std::error_code exists_ec;
    if (!std::filesystem::is_regular_file(c, exists_ec)) continue;
    auto loaded = load_native_manifest(c);
    if (loaded.ok) return loaded;
    last = std::move(loaded);
  }
  return last;
}

std::filesystem::path default_native_model_dir() {
  return expand_user_path("~/.cache/sublift/models/ppocrv6-small");
}

std::filesystem::path default_legacy_model_dir() {
  return expand_user_path("~/.cache/sublift/rapidocr-models");
}

std::filesystem::path default_ort_cache_dir() {
  return expand_user_path("~/.cache/sublift/onnxruntime");
}

std::string current_os_name() {
#if defined(__APPLE__)
  return "darwin";
#elif defined(__linux__)
  return "linux";
#else
  return "unknown";
#endif
}

std::string current_arch_name() {
#if defined(__aarch64__) || defined(__arm64__)
  return "arm64";
#elif defined(__x86_64__)
  return "x64";
#else
  return "unknown";
#endif
}

const OrtArtifactSpec* select_ort_artifact(
    const NativeResourceManifest& manifest, std::string_view os, std::string_view arch) {
  const OrtArtifactSpec* first = nullptr;
  for (const auto& art : manifest.onnxruntime) {
    if (art.os != os || art.arch != arch) continue;
    if (first == nullptr) first = &art;
    if (!art.url.empty()) return &art;
  }
  return first;
}

bool ort_library_digest_allowed(
    const NativeResourceManifest& manifest, const std::filesystem::path& library_path,
    std::string* error_msg) {
  if (is_python_packaged_path(library_path)) {
    if (error_msg) {
      *error_msg =
          "ONNX Runtime library is a Python wheel path and is not a product source: " +
          library_path.string();
    }
    return false;
  }
  auto digest = sha256_file_hex(library_path);
  if (!digest.has_value()) {
    if (error_msg) {
      *error_msg = "cannot hash ONNX Runtime library: " + library_path.string();
    }
    return false;
  }
  for (const auto& art : manifest.onnxruntime) {
    if (art.sha256 == *digest) return true;
  }
  if (error_msg) {
    *error_msg = "ONNX Runtime library SHA-256 " + *digest +
                 " is not in native-resources/manifest.v1.json";
  }
  return false;
}

}  // namespace sublift::models
