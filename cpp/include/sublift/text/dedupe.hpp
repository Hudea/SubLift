#pragma once

#include <cstdint>
#include <vector>
#include <sublift/core/subtitle.hpp>

namespace sublift::text {

[[nodiscard]] std::vector<SubtitleEntry> merge_entries(
    const std::vector<SubtitleEntry>& entries,
    std::int64_t merge_gap_ms = 1000,
    std::int64_t min_duration_ms = 500,
    bool drop_empty_text = false);

} // namespace sublift::text
