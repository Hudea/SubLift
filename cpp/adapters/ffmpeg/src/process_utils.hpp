#pragma once

#include <chrono>
#include <atomic>
#include <functional>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sublift::adapters::ffmpeg::detail {

struct SubprocessResult {
  int exit_code{-1};
  bool timed_out{false};
  bool cancelled{false};
  std::string stdout_str;
  std::string stderr_str;
};

[[nodiscard]] SubprocessResult run_subprocess(
    const std::vector<std::string>& args,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(60000),
    const std::atomic<bool>* cancelled = nullptr,
    const std::function<void(std::string_view)>& stdout_chunk = {},
    const std::function<bool()>& should_cancel = {});

[[nodiscard]] std::string read_stderr_tail(std::string_view stderr_str,
                                           std::size_t max_chars = 2000);

}  // namespace sublift::adapters::ffmpeg::detail
