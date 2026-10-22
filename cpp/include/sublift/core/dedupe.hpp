#pragma once

#include <cstdint>
#include <vector>

#include "sublift/models.hpp"

namespace sublift {

/// Matches Python `merge_entries` (4-pass dedupe/merge).
[[nodiscard]] std::vector<SubtitleEntry> merge_entries(
    const std::vector<SubtitleEntry>& entries, std::int32_t merge_gap_ms = 1000,
    std::int32_t min_duration_ms = 500, bool drop_empty_text = false);

/// Entry-level persistent-watermark suppression (Phase 21 D03).
///
/// A rolling/static overlay (e.g. "暖暖的猫 98Tang.COM") is present in ROI on
/// every frame, so the binder keeps re-reading it in dialogue-free gaps and
/// would otherwise emit watermark cues across the whole film. This filter
/// removes a whole "watermark family" from raw binder entries:
///
/// - **Candidate subset** — non-empty entries whose whitespace-stripped text is
///   URL/domain-like (`is_watermark_text`) or short (<= 12 code points).
///   Empty/blank entries are never candidates, so the existing blank-cue merge
///   behaviour is untouched.
/// - **Family grouping** — union-find over the candidate subset only (bounded
///   O(m^2), m <= 4096; above the bound the filter fails safe). Two candidates
///   are in the same family when their normalized texts are equal, match the
///   existing `merge_entries` containment/subsequence classifier, or are
///   within bounded edit distance (one-character OCR variants of one overlay).
/// - **Removal** — a family is removed when it has >= 5 entries, the union of
///   its entry intervals covers >= 8% of `video_duration_ms`, and its time
///   span is >= 60% of `video_duration_ms`. All three are required, so a
///   normal short line repeated a few times (e.g. "谢谢") is preserved.
/// - **Safety guard** — the family must carry a watermark signal: at least one
///   URL/domain/mixed-token text, or >= 2 distinct OCR variants of the same
///   text. An exact-identical family of normal dialogue (looped source clip,
///   repeated lyric/short cue whose aggregate coverage happens to pass the
///   thresholds) is kept, because it has no URL evidence and only one variant.
///   See the Phase 21 128s loop A/B.
///
/// Pure and deterministic: no I/O, no env, no Config change; relative input
/// order is preserved in the result. `Pipeline::finalize` calls this on the
/// raw closed entries *before* `merge_entries`; removed entries never reach the
/// final output. `video_duration_ms <= 0` is a no-op.
[[nodiscard]] std::vector<SubtitleEntry> suppress_watermark_entries(
    const std::vector<SubtitleEntry>& entries, std::int64_t video_duration_ms);

}  // namespace sublift
