#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <stdexcept>
#include <string>

#include "sublift/ffmpeg.hpp"

#include <unistd.h>

namespace sublift::ffmpeg {

namespace {

const std::vector<std::string> FFMPEG_CANDIDATES = {
    "ffmpeg",
    "/opt/homebrew/opt/ffmpeg-full/bin/ffmpeg",
    "/opt/homebrew/bin/ffmpeg",
    "/usr/local/bin/ffmpeg",
};

const std::vector<std::string> FFPROBE_CANDIDATES = {
    "ffprobe",
    "/opt/homebrew/opt/ffmpeg-full/bin/ffprobe",
    "/opt/homebrew/bin/ffprobe",
    "/usr/local/bin/ffprobe",
};

bool is_executable_file(const std::filesystem::path& p) {
  std::error_code ec;
  if (!std::filesystem::exists(p, ec) || !std::filesystem::is_regular_file(p, ec)) {
    return false;
  }
  return ::access(p.c_str(), X_OK) == 0;
}

std::string resolve_bin(const std::vector<std::string>& candidates,
                        const std::string& label) {
  for (const auto& name : candidates) {
    if (name.rfind('/', 0) == 0) {  // starts with '/'
      std::filesystem::path p{name};
      if (is_executable_file(p)) {
        return p.string();
      }
    } else {
      const char* path_env = std::getenv("PATH");
      if (path_env != nullptr) {
        std::string env_str{path_env};
        std::stringstream ss{env_str};
        std::string dir;
        while (std::getline(ss, dir, ':')) {
          if (dir.empty()) continue;
          std::filesystem::path candidate_path = std::filesystem::path{dir} / name;
          if (is_executable_file(candidate_path)) {
            return candidate_path.string();
          }
        }
      }
    }
  }

  std::string tried;
  for (size_t i = 0; i < candidates.size(); ++i) {
    if (i > 0) tried += ", ";
    tried += candidates[i];
  }

  throw std::runtime_error("未找到 " + label + "，请安装 ffmpeg 并确保在 PATH 中（已试: " +
                           tried + "）");
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
  return resolve_bin(FFMPEG_CANDIDATES, "ffmpeg");
}

std::string resolve_ffprobe_bin() {
  return resolve_bin(FFPROBE_CANDIDATES, "ffprobe");
}

}  // namespace sublift::ffmpeg
