#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace sublift::models {

struct ResourceFileSpec {
  std::string role;
  std::string filename;
  std::string sha256;
  std::string url;
};

struct PaddleBundleSpec {
  std::string id{"ppocrv6-small"};
  std::string version;
  std::string install_subdir{"models/ppocrv6-small"};
  std::vector<ResourceFileSpec> files;
};

struct OrtArtifactSpec {
  std::string os;
  std::string arch;
  std::string version;
  std::string library;
  std::string sha256;
  std::string url;
  std::string tarball_sha256;
  std::string source;
};

struct NativeResourceManifest {
  int schema_version{1};
  std::string kind{"native-resources"};
  PaddleBundleSpec paddle;
  std::vector<OrtArtifactSpec> onnxruntime;
};

struct ManifestLoadResult {
  bool ok{false};
  NativeResourceManifest manifest{};
  std::filesystem::path path;
  std::string error_msg;
};

[[nodiscard]] bool is_python_packaged_path(const std::filesystem::path& path);

[[nodiscard]] ManifestLoadResult load_native_manifest(const std::filesystem::path& path);

/// Search env, compile-time path, executable-adjacent share dir, then repo tree.
[[nodiscard]] ManifestLoadResult load_default_native_manifest(
    const std::optional<std::filesystem::path>& executable_dir = std::nullopt);

[[nodiscard]] std::filesystem::path default_native_model_dir();
[[nodiscard]] std::filesystem::path default_legacy_model_dir();
[[nodiscard]] std::filesystem::path default_ort_cache_dir();

[[nodiscard]] std::string current_os_name();
[[nodiscard]] std::string current_arch_name();

[[nodiscard]] const OrtArtifactSpec* select_ort_artifact(
    const NativeResourceManifest& manifest, std::string_view os, std::string_view arch);

[[nodiscard]] bool ort_library_digest_allowed(
    const NativeResourceManifest& manifest, const std::filesystem::path& library_path,
    std::string* error_msg = nullptr);

}  // namespace sublift::models
