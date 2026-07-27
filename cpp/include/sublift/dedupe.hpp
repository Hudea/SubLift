#pragma once

#include <cstdint>
#include <vector>

#include "sublift/models.hpp"

namespace sublift {

/// Matches Python `merge_entries` (4-pass dedupe/merge).
[[nodiscard]] std::vector<SubtitleEntry> merge_entries(
    const std::vector<SubtitleEntry>& entries, std::int32_t merge_gap_ms = 1000,
    std::int32_t min_duration_ms = 500, bool drop_empty_text = false);

}  // namespace sublift
