#pragma once

#include <algorithm>
#include <cstdlib>
#include <mutex>

#include <opencv2/core.hpp>

namespace sublift::features::detail {

/// 将 OpenCV 内部线程池固定为单线程（可通过 SUBLIFT_OPENCV_THREADS 覆盖），
/// 消除上层并行分析线程与 OpenCV 内部并行的 N*M 线程超订阅与上下文切换抖动。
inline void ensure_opencv_single_thread() noexcept {
  static std::once_flag once;
  std::call_once(once, []() noexcept {
    int threads = 1;
    if (const char* env = std::getenv("SUBLIFT_OPENCV_THREADS");
        env != nullptr && *env != '\0') {
      char* end = nullptr;
      const long parsed = std::strtol(env, &end, 10);
      if (end != env && *end == '\0' && parsed >= 0) {
        threads = static_cast<int>(std::clamp(parsed, 0L, 64L));
      }
    }
    cv::setNumThreads(threads);
  });
}

} // namespace sublift::features::detail
