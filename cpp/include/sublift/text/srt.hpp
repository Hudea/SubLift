#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sublift/core/subtitle.hpp>

namespace sublift::text {

/// Formats millisecond timestamp into SRT timestamp "hh:mm:ss,ms" (negative clamped to 0)
[[nodiscard]] std::string format_timestamp_srt(std::int64_t timestamp_ms);

/// Alias matching SubLift core/srt.hpp
[[nodiscard]] inline std::string format_srt_timestamp(std::int64_t ms) {
  return format_timestamp_srt(ms);
}

/// Checks if string consists entirely of whitespace
[[nodiscard]] bool is_blank_text(std::string_view text);

/// Formats subtitle entries into valid SRT format with 1-based sequential cue indices
[[nodiscard]] std::string format_srt(const std::vector<SubtitleEntry>& entries);

/// Formats SRT and returns formatted string and written cue count
[[nodiscard]] std::pair<std::string, std::size_t> format_srt_counted(
    const std::vector<SubtitleEntry>& entries);

} // namespace sublift::text
