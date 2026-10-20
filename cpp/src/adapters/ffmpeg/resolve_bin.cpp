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
  // Process-level cache (O7): the resolution scans PATH, and extract() plus
  // the single-frame APIs used to repeat it per call. PATH is treated as
  // immutable within a process lifetime (documented); SUBLIFT_FFMPEG_PATH
  // overrides are resolved on the first call like any other candidate.
  // A failed resolution is not cached (the static initializer re-runs).
  static const std::string cached = [] {
    models::ResourceLocator locator;
    auto res = locator.locate_ffmpeg_executable("");
    if (!res.found) {
      throw std::runtime_error(
          "未找到 ffmpeg，请安装并确保在 PATH 中，或设置 SUBLIFT_FFMPEG_PATH（" +
          res.error_msg + "）");
    }
    return res.value.string();
  }();
  return cached;
}

std::string resolve_ffprobe_bin() {
  // See resolve_ffmpeg_bin for the caching contract (O7).
  static const std::string cached = [] {
    models::ResourceLocator locator;
    auto res = locator.locate_ffprobe_executable("");
    if (!res.found) {
      throw std::runtime_error(
          "未找到 ffprobe，请安装并确保在 PATH 中，或设置 SUBLIFT_FFPROBE_PATH（" +
          res.error_msg + "）");
    }
    return res.value.string();
  }();
  return cached;
}

}  // namespace sublift::ffmpeg
