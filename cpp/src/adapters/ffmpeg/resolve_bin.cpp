#include <stdexcept>
#include <string>

#include "sublift/adapters/ffmpeg.hpp"
#include "sublift/models/resource_locator.hpp"

namespace sublift::ffmpeg {

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
  models::ResourceLocator locator;
  auto res = locator.locate_ffmpeg_executable("");
  if (!res.found) {
    throw std::runtime_error(
        "未找到 ffmpeg，请安装并确保在 PATH 中，或设置 SUBLIFT_FFMPEG_PATH（" +
        res.error_msg + "）");
  }
  return res.value.string();
}

std::string resolve_ffprobe_bin() {
  models::ResourceLocator locator;
  auto res = locator.locate_ffprobe_executable("");
  if (!res.found) {
    throw std::runtime_error(
        "未找到 ffprobe，请安装并确保在 PATH 中，或设置 SUBLIFT_FFPROBE_PATH（" +
        res.error_msg + "）");
  }
  return res.value.string();
}

}  // namespace sublift::ffmpeg
