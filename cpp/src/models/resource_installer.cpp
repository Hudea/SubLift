#include "sublift/models/resource_installer.hpp"

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdlib>
#include <fstream>
#include <sstream>
#include <vector>

#include "sublift/hash.hpp"

extern "C" char** environ;

namespace sublift::models {
namespace {

[[nodiscard]] bool exists_regular(const std::filesystem::path& p) {
  std::error_code ec;
  return std::filesystem::is_regular_file(p, ec);
}

[[nodiscard]] std::optional<std::filesystem::path> find_on_path(const std::string& name) {
  const char* path_env = std::getenv("PATH");
  if (path_env == nullptr || *path_env == '\0') return std::nullopt;
  std::stringstream ss{path_env};
  std::string dir;
  while (std::getline(ss, dir, ':')) {
    if (dir.empty()) continue;
    auto candidate = std::filesystem::path{dir} / name;
    if (exists_regular(candidate) && ::access(candidate.c_str(), X_OK) == 0) {
      return candidate;
    }
  }
  return std::nullopt;
}

int run_argv(const std::vector<std::string>& args) {
  if (args.empty()) return -1;
  std::vector<char*> argv;
  argv.reserve(args.size() + 1);
  for (const auto& arg : args) {
    argv.push_back(const_cast<char*>(arg.c_str()));
  }
  argv.push_back(nullptr);

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
  const int devnull_fd = ::open("/dev/null", O_RDWR);
  if (devnull_fd >= 0) {
    posix_spawn_file_actions_adddup2(&actions, devnull_fd, STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, devnull_fd, STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, devnull_fd, STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, devnull_fd);
  }

  pid_t pid = 0;
  const int spawn_err =
      ::posix_spawn(&pid, args.front().c_str(), &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  if (devnull_fd >= 0) {
    ::close(devnull_fd);
  }
  if (spawn_err != 0) return -1;

  int status = 0;
  if (::waitpid(pid, &status, 0) < 0) return -1;
  if (WIFEXITED(status)) return WEXITSTATUS(status);
  return -1;
}

bool download_to(const std::string& url, const std::filesystem::path& dest, std::string* error) {
  if (auto curl = find_on_path("curl")) {
    if (run_argv({curl->string(), "-fsSL", "--retry", "3", "--retry-delay", "1", "-o",
                  dest.string(), url}) == 0 &&
        exists_regular(dest)) {
      return true;
    }
    *error = "curl download failed for " + url;
    return false;
  }
  if (auto wget = find_on_path("wget")) {
    if (run_argv({wget->string(), "-q", "-T", "60", "--tries=3", "-O", dest.string(), url}) == 0 &&
        exists_regular(dest)) {
      return true;
    }
    *error = "wget download failed for " + url;
    return false;
  }
  *error = "neither curl nor wget is available to fetch " + url;
  return false;
}

bool copy_file_bytes(const std::filesystem::path& from, const std::filesystem::path& to,
                     std::string* error) {
  std::error_code ec;
  std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing, ec);
  if (ec) {
    *error = "copy failed: " + ec.message();
    return false;
  }
  return true;
}

bool atomic_replace(const std::filesystem::path& tmp, const std::filesystem::path& dest,
                    std::string* error) {
  std::error_code ec;
  std::filesystem::rename(tmp, dest, ec);
  if (!ec) return true;
  std::filesystem::remove(dest, ec);
  std::filesystem::rename(tmp, dest, ec);
  if (ec) {
    *error = "atomic replace failed: " + ec.message();
    return false;
  }
  return true;
}

}  // namespace

InstallResult install_paddle_bundle(
    const NativeResourceManifest& manifest, const InstallOptions& options) {
  InstallResult result;
  auto dest_dir = options.destination_dir.empty() ? default_native_model_dir()
                                                  : options.destination_dir;
  result.destination_dir = dest_dir;
  std::error_code ec;
  std::filesystem::create_directories(dest_dir, ec);
  if (ec) {
    result.error_msg = "cannot create model directory: " + dest_dir.string();
    return result;
  }

  bool all_ok = true;
  for (const auto& spec : manifest.paddle.files) {
    InstallFileResult file;
    file.filename = spec.filename;
    const auto dest = dest_dir / spec.filename;
    if (exists_regular(dest)) {
      const auto digest = sha256_file_hex(dest);
      if (digest && *digest == spec.sha256) {
        file.ok = true;
        file.skipped = true;
        file.detail = "already verified";
        result.files.push_back(std::move(file));
        continue;
      }
    }

    const auto tmp = dest_dir / (spec.filename + ".partial." + std::to_string(::getpid()));
    std::filesystem::remove(tmp, ec);

    bool staged = false;
    std::string error;
    for (const auto& source_dir : options.source_dirs) {
      const auto candidate = source_dir / spec.filename;
      if (!exists_regular(candidate)) continue;
      const auto digest = sha256_file_hex(candidate);
      if (!digest || *digest != spec.sha256) continue;
      if (!copy_file_bytes(candidate, tmp, &error)) {
        std::filesystem::remove(tmp, ec);
        continue;
      }
      staged = true;
      file.detail = "copied from " + candidate.string();
      break;
    }

    if (!staged && options.allow_network && !spec.url.empty()) {
      if (download_to(spec.url, tmp, &error)) {
        staged = true;
        file.detail = "downloaded";
      }
    }

    if (!staged) {
      std::filesystem::remove(tmp, ec);
      file.ok = false;
      file.detail = error.empty() ? "source not found and network fetch disabled or failed"
                                  : error;
      all_ok = false;
      result.files.push_back(std::move(file));
      continue;
    }

    const auto digest = sha256_file_hex(tmp);
    if (!digest || *digest != spec.sha256) {
      std::filesystem::remove(tmp, ec);
      file.ok = false;
      file.detail = "SHA-256 mismatch for " + spec.filename +
                    "; partial file removed (got " + digest.value_or("unreadable") + ")";
      all_ok = false;
      result.files.push_back(std::move(file));
      continue;
    }

    if (!atomic_replace(tmp, dest, &error)) {
      std::filesystem::remove(tmp, ec);
      file.ok = false;
      file.detail = error;
      all_ok = false;
      result.files.push_back(std::move(file));
      continue;
    }
    file.ok = true;
    result.files.push_back(std::move(file));
  }

  result.ok = all_ok;
  if (!all_ok) {
    result.error_msg = "paddle bundle install failed; incomplete files were not published";
  }
  return result;
}

}  // namespace sublift::models
