#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

#include <sublift/adapters/ffmpeg/ffmpeg_frame_source.hpp>

namespace sublift::adapters::ffmpeg {
namespace {

bool is_executable_file(const std::filesystem::path& p) {
  std::error_code ec;
  if (!std::filesystem::is_regular_file(p, ec) && !std::filesystem::is_symlink(p, ec)) {
    return false;
  }
  return ::access(p.c_str(), X_OK) == 0;
}

std::optional<std::filesystem::path> find_on_path(const std::string& name) {
  const char* path_env = std::getenv("PATH");
  if (path_env == nullptr || *path_env == '\0') {
    return std::nullopt;
  }

  std::string_view sv(path_env);
  std::size_t start = 0;
  while (start < sv.size()) {
    std::size_t end = sv.find(':', start);
    if (end == std::string_view::npos) {
      end = sv.size();
    }
    std::filesystem::path dir(sv.substr(start, end - start));
    if (!dir.empty()) {
      auto full = dir / name;
      if (is_executable_file(full)) {
        return full;
      }
    }
    start = end + 1;
  }
  return std::nullopt;
}

std::filesystem::path expand_user_path(std::string_view path_str) {
  if (path_str.empty()) {
    return {};
  }
  if (path_str[0] == '~') {
    const char* home = std::getenv("HOME");
    if (home != nullptr) {
      return std::filesystem::path(home) / path_str.substr(1 < path_str.size() && path_str[1] == '/' ? 2 : 1);
    }
  }
  return std::filesystem::path(path_str);
}

}  // namespace

bool available() noexcept {
  try {
    static_cast<void>(resolve_ffmpeg_bin());
    static_cast<void>(resolve_ffprobe_bin());
    return true;
  } catch (...) {
    return false;
  }
}

std::string resolve_ffmpeg_bin() {
  static const std::string cached = [] {
    if (const char* env = std::getenv("SUBLIFT_FFMPEG_PATH"); env != nullptr && *env != '\0') {
      auto p = expand_user_path(env);
      if (is_executable_file(p)) {
        return p.string();
      }
      throw std::runtime_error("FFmpeg executable not found at SUBLIFT_FFMPEG_PATH: " + p.string());
    }

    if (auto on_path = find_on_path("ffmpeg"); on_path.has_value()) {
      return on_path->string();
    }

    const std::vector<std::filesystem::path> candidates = {
        "/opt/homebrew/opt/ffmpeg-full/bin/ffmpeg",
        "/opt/homebrew/bin/ffmpeg",
        "/usr/local/bin/ffmpeg",
        "/usr/bin/ffmpeg",
    };
    for (const auto& c : candidates) {
      if (is_executable_file(c)) {
        return c.string();
      }
    }

    throw std::runtime_error("未找到 ffmpeg，请安装并确保在 PATH 中，或设置 SUBLIFT_FFMPEG_PATH");
  }();
  return cached;
}

std::string resolve_ffprobe_bin() {
  static const std::string cached = [] {
    if (const char* env = std::getenv("SUBLIFT_FFPROBE_PATH"); env != nullptr && *env != '\0') {
      auto p = expand_user_path(env);
      if (is_executable_file(p)) {
        return p.string();
      }
      throw std::runtime_error("ffprobe executable not found at SUBLIFT_FFPROBE_PATH: " + p.string());
    }

    if (auto on_path = find_on_path("ffprobe"); on_path.has_value()) {
      return on_path->string();
    }

    const std::vector<std::filesystem::path> candidates = {
        "/opt/homebrew/opt/ffmpeg-full/bin/ffprobe",
        "/opt/homebrew/bin/ffprobe",
        "/usr/local/bin/ffprobe",
        "/usr/bin/ffprobe",
    };
    for (const auto& c : candidates) {
      if (is_executable_file(c)) {
        return c.string();
      }
    }

    throw std::runtime_error("未找到 ffprobe，请安装并确保在 PATH 中，或设置 SUBLIFT_FFPROBE_PATH");
  }();
  return cached;
}

}  // namespace sublift::adapters::ffmpeg
