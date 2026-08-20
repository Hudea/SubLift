#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sublift/models.hpp"

namespace sublift::cli {

[[nodiscard]] std::string format_srt_timestamp(std::int64_t ms);
[[nodiscard]] bool is_blank_text(std::string_view text);

/// Format SRT; skips blank-text entries. Returns body and written cue count.
[[nodiscard]] std::pair<std::string, std::size_t> format_srt(
    const std::vector<sublift::SubtitleEntry>& entries);

}  // namespace sublift::cli
