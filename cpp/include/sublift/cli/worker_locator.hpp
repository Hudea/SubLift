#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace sublift::cli {

struct WorkerLocateContext {
  std::optional<std::filesystem::path> override_path;
  std::optional<std::filesystem::path> env_path;
  std::optional<std::filesystem::path> executable_dir;
  std::optional<std::filesystem::path> cwd;
};

/// Resolve `sublift_worker`. An explicit override or env path that is missing
/// fails closed and does not fall through to another candidate.
[[nodiscard]] std::optional<std::filesystem::path> resolve_worker(
    const WorkerLocateContext& ctx);

[[nodiscard]] std::optional<std::filesystem::path> process_executable_dir();

/// Locate the worker using process env, this executable's directory, and cwd.
[[nodiscard]] std::optional<std::filesystem::path> resolve_worker_from_process(
    const std::optional<std::filesystem::path>& override_path);

[[nodiscard]] std::vector<std::filesystem::path> default_cwd_candidates(
    const std::filesystem::path& cwd);

}  // namespace sublift::cli
