#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sublift/models.hpp"

namespace sublift {

struct LineScore {
  double total{0.0};
  double script{0.0};
  double y_band{0.0};
  double height{0.0};
  double center{0.0};
  double confidence{0.0};

  friend constexpr bool operator==(const LineScore&, const LineScore&) = default;
};

struct ConsensusResult {
  std::string text;
  double confidence{0.0};
  int support_votes{0};

  friend bool operator==(const ConsensusResult&, const ConsensusResult&) = default;
};

[[nodiscard]] std::string normalize_ocr_text(std::string_view text);
[[nodiscard]] std::string cleanup_subtitle_text(std::string_view text,
                                                std::string_view script = SCRIPT_CJK);

[[nodiscard]] double cjk_ratio(std::string_view text);
[[nodiscard]] double latin_ratio(std::string_view text);
[[nodiscard]] double script_score(std::string_view text, std::string_view script);

[[nodiscard]] LineScore score_line(const OcrLine& line, const SubtitleProfile& profile);

[[nodiscard]] std::optional<OcrLine> select_line(
    const std::vector<OcrLine>& lines, const SubtitleProfile& profile,
    double min_score = 0.28, double min_script = 0.12);

/// True when `text` looks like a persistent watermark/URL overlay rather than
/// subtitle dialogue (Phase 21 D03 line-level rule).
///
/// The rule set is deliberately narrow so normal dialogue (including mixed
/// CJK/Latin lines such as "任务代号 FOX TWO") is never affected:
///   1. explicit URL scheme: `http://` / `https://`;
///   2. a `www.` token;
///   3. a `label.tld` domain token (ASCII, case-insensitive; label >= 2 ASCII
///      chars, tld 2-6 ASCII letters from the common watermark TLD list, e.g.
///      com/net/org/cn/cc/tv/...). Rules 1-3 reuse the single domain
///      definition shared with the D02 band detector;
///   4. short CJK line with a URL-ish mixed-script token: ASCII digits directly
///      followed by >= 2 ASCII letters ("暖暖的猫 98Ta", "暖暖的猫98Tang"), i.e.
///      an overlay clipped before its domain dot. Normal "第2集", "1080P",
///      "iPhone14", "X战警2" and pure-Latin "20th century" are unaffected.
///
/// `select_line` adds two frame-context rules on top of this predicate:
///   - a URL-bearing line is still usable when `cleanup_subtitle_text` can
///     strip the edge-attached overlay and leaves CJK dialogue (OCR glues the
///     scrolling overlay to the subtitle: "第三句台词内容Tang.COM" ->
///     "第三句台词内容"); otherwise it is unselectable;
///   - a short pure-CJK line that matches a URL line's font size/position in
///     the same frame (overlay split into two bboxes: "暖暖的猫" +
///     "苗98Tang.COM") is treated as an overlay fragment.
///
/// Pure-CJK watermark fragments without any frame/cleanup evidence (e.g. the
/// nickname alone, when nothing else in frame identifies the overlay) cannot
/// be told apart from ordinary CJK dialogue by text alone; those are covered
/// by the entry-level full-film repetition filter
/// (`suppress_watermark_entries` in dedupe.hpp).
///
/// `score_line` caps such a line's total score below the default `min_score`,
/// so any caller that aggregates raw scores also rejects it.
[[nodiscard]] bool is_watermark_text(std::string_view text);

[[nodiscard]] int edit_distance(std::string_view a, std::string_view b);
[[nodiscard]] bool is_similar(std::string_view a, std::string_view b);

[[nodiscard]] ConsensusResult consensus_text(
    const std::vector<std::pair<std::string, double>>& samples,
    std::string_view script = SCRIPT_AUTO);

[[nodiscard]] bool should_accept_text(
    std::string_view text, double confidence, const SubtitleProfile& profile,
    double confidence_threshold, double low_conf_threshold, int support_votes,
    int min_stable_votes = 2);

}  // namespace sublift
