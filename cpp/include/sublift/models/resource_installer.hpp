#pragma once

#include <filesystem>
#include <string>
#include <vector>

#include "sublift/models/native_manifest.hpp"

namespace sublift::models {

struct InstallOptions {
  std::filesystem::path destination_dir;
  std::vector<std::filesystem::path> source_dirs;
  bool allow_network{true};
};

struct InstallFileResult {
  std::string filename;
  bool ok{false};
  bool skipped{false};
  std::string detail;
};

struct InstallResult {
  bool ok{false};
  std::filesystem::path destination_dir;
  std::vector<InstallFileResult> files;
  std::string error_msg;
};

/// Copy or download each paddle file into destination_dir. Writes a `.partial`
/// file, verifies SHA-256, then atomically replaces the destination. Failures
/// delete the partial file and never leave a digest-valid destination.
[[nodiscard]] InstallResult install_paddle_bundle(
    const NativeResourceManifest& manifest, const InstallOptions& options);

}  // namespace sublift::models
