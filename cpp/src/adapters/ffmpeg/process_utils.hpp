#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace sublift::ffmpeg::detail {

struct SubprocessResult {
  int exit_code{-1};
  bool timed_out{false};
  std::string stdout_str;
  std::string stderr_str;
};

[[nodiscard]] SubprocessResult run_subprocess(
    const std::vector<std::string>& args,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(60000));

[[nodiscard]] std::string read_stderr_tail(std::string_view stderr_str,
                                           std::size_t max_chars = 2000);

}  // namespace sublift::ffmpeg::detail
