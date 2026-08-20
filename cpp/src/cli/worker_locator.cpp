#include "sublift/cli/worker_locator.hpp"

#include <cstdlib>

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

namespace sublift::cli {
namespace {

[[nodiscard]] bool exists_file(const std::filesystem::path& p) {
  std::error_code ec;
  return std::filesystem::exists(p, ec) && !std::filesystem::is_directory(p, ec);
}

}  // namespace

std::vector<std::filesystem::path> default_cwd_candidates(
    const std::filesystem::path& cwd) {
  return {
      cwd / "build" / "cpp-rel" / "bin" / "sublift_worker",
      cwd / "build" / "cpp" / "bin" / "sublift_worker",
      cwd / "bin" / "sublift_worker",
      cwd / "build/cpp-rel/bin/sublift_worker",
      cwd / "build/cpp/bin/sublift_worker",
  };
}

std::optional<std::filesystem::path> process_executable_dir() {
#if defined(__APPLE__)
  char buf[4096];
  uint32_t size = sizeof(buf);
  if (_NSGetExecutablePath(buf, &size) != 0) return std::nullopt;
  std::error_code ec;
  std::filesystem::path p = std::filesystem::weakly_canonical(std::filesystem::path{buf}, ec);
  if (ec) p = std::filesystem::path{buf};
  return p.parent_path();
#else
  std::error_code ec;
  std::filesystem::path self = std::filesystem::read_symlink("/proc/self/exe", ec);
  if (ec) return std::nullopt;
  return self.parent_path();
#endif
}

std::optional<std::filesystem::path> resolve_worker(const WorkerLocateContext& ctx) {
  if (ctx.override_path.has_value()) {
    if (exists_file(*ctx.override_path)) return *ctx.override_path;
    return std::nullopt;
  }
  if (ctx.env_path.has_value()) {
    if (exists_file(*ctx.env_path)) return *ctx.env_path;
    return std::nullopt;
  }
  if (ctx.executable_dir.has_value()) {
    const auto& dir = *ctx.executable_dir;
    const std::filesystem::path sibling = dir / "sublift_worker";
    if (exists_file(sibling)) return sibling;
    if (dir.filename() == "MacOS" && dir.parent_path().filename() == "Contents") {
      const std::filesystem::path helper =
          dir.parent_path() / "Helpers" / "sublift_worker";
      if (exists_file(helper)) return helper;
    }
  }
  if (ctx.cwd.has_value()) {
    for (const auto& c : default_cwd_candidates(*ctx.cwd)) {
      if (exists_file(c)) return std::filesystem::absolute(c);
    }
  }
  return std::nullopt;
}

std::optional<std::filesystem::path> resolve_worker_from_process(
    const std::optional<std::filesystem::path>& override_path) {
  WorkerLocateContext ctx;
  ctx.override_path = override_path;
  if (const char* env = std::getenv("SUBLIFT_WORKER_PATH");
      env != nullptr && *env != '\0') {
    ctx.env_path = std::filesystem::path{env};
  }
  ctx.executable_dir = process_executable_dir();
  ctx.cwd = std::filesystem::current_path();
  return resolve_worker(ctx);
}

}  // namespace sublift::cli
