#include "sublift/dedupe.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "sublift/line_select.hpp"

namespace sublift {
namespace {

constexpr std::size_t kSimilarityMinCodePoints = 4;
constexpr std::size_t kSimilarityMinRatioPercent = 60;
constexpr std::int64_t kSimilarityMaxGapMs = 300;

/// Match Python `str.isspace()` for common Zs + ASCII (same set as line_select).
[[nodiscard]] bool is_python_space_byte_or_utf8(const std::string& text,
                                                std::size_t& i) {
  if (i >= text.size()) {
    return false;
  }
  const auto c = static_cast<unsigned char>(text[i]);
  if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
    ++i;
    return true;
  }
  // UTF-8 multi-byte spaces: U+00A0, U+3000, U+2000-200A, etc.
  auto peek_cp = [&](char32_t& out) -> bool {
    const auto c0 = static_cast<unsigned char>(text[i]);
    if (c0 < 0x80) {
      out = c0;
      ++i;
      return true;
    }
    if ((c0 & 0xE0) == 0xC0 && i + 1 < text.size()) {
      out = (static_cast<char32_t>(c0 & 0x1F) << 6) |
            (static_cast<unsigned char>(text[i + 1]) & 0x3F);
      i += 2;
      return true;
    }
    if ((c0 & 0xF0) == 0xE0 && i + 2 < text.size()) {
      out = (static_cast<char32_t>(c0 & 0x0F) << 12) |
            ((static_cast<unsigned char>(text[i + 1]) & 0x3F) << 6) |
            (static_cast<unsigned char>(text[i + 2]) & 0x3F);
      i += 3;
      return true;
    }
    return false;
  };
  const std::size_t save = i;
  char32_t cp = 0;
  if (!peek_cp(cp)) {
    return false;
  }
  const bool space =
      cp == 0x00A0 || cp == 0x1680 || cp == 0x2028 || cp == 0x2029 ||
      cp == 0x202F || cp == 0x205F || cp == 0x3000 ||
      (cp >= 0x2000 && cp <= 0x200A);
  if (!space) {
    i = save;
    return false;
  }
  return true;
}

[[nodiscard]] std::string normalize_for_merge(const std::string& text) {
  // Python: "".join(text.split()) — remove all whitespace (Unicode).
  std::string out;
  out.reserve(text.size());
  std::size_t i = 0;
  while (i < text.size()) {
    const std::size_t before = i;
    if (is_python_space_byte_or_utf8(text, i)) {
      continue;
    }
    // copy one UTF-8 scalar or byte
    if (i == before) {
      const auto c0 = static_cast<unsigned char>(text[i]);
      if (c0 < 0x80) {
        out.push_back(static_cast<char>(c0));
        ++i;
      } else if ((c0 & 0xE0) == 0xC0 && i + 1 < text.size()) {
        out.append(text, i, 2);
        i += 2;
      } else if ((c0 & 0xF0) == 0xE0 && i + 2 < text.size()) {
        out.append(text, i, 3);
        i += 3;
      } else if ((c0 & 0xF8) == 0xF0 && i + 3 < text.size()) {
        out.append(text, i, 4);
        i += 4;
      } else {
        out.push_back(static_cast<char>(c0));
        ++i;
      }
    }
  }
  return out;
}

[[nodiscard]] std::size_t utf8_cp_count(const std::string& text) {
  std::size_t n = 0;
  for (std::size_t i = 0; i < text.size();) {
    const auto c = static_cast<unsigned char>(text[i]);
    if ((c & 0x80) == 0) {
      i += 1;
    } else if ((c & 0xE0) == 0xC0) {
      i += 2;
    } else if ((c & 0xF0) == 0xE0) {
      i += 3;
    } else if ((c & 0xF8) == 0xF0) {
      i += 4;
    } else {
      i += 1;
    }
    if (i > text.size()) break;
    ++n;
  }
  return n;
}

enum class MergeKind { kNone, kIdentical, kContainment, kSimilar };

[[nodiscard]] std::vector<char32_t> utf8_codepoints(const std::string& text) {
  std::vector<char32_t> out;
  out.reserve(text.size());
  for (std::size_t i = 0; i < text.size();) {
    const auto c0 = static_cast<unsigned char>(text[i]);
    char32_t cp = c0;
    std::size_t length = 1;
    if ((c0 & 0xE0) == 0xC0 && i + 1 < text.size()) {
      cp = (static_cast<char32_t>(c0 & 0x1F) << 6) |
           (static_cast<unsigned char>(text[i + 1]) & 0x3F);
      length = 2;
    } else if ((c0 & 0xF0) == 0xE0 && i + 2 < text.size()) {
      cp = (static_cast<char32_t>(c0 & 0x0F) << 12) |
           ((static_cast<unsigned char>(text[i + 1]) & 0x3F) << 6) |
           (static_cast<unsigned char>(text[i + 2]) & 0x3F);
      length = 3;
    } else if ((c0 & 0xF8) == 0xF0 && i + 3 < text.size()) {
      cp = (static_cast<char32_t>(c0 & 0x07) << 18) |
           ((static_cast<unsigned char>(text[i + 1]) & 0x3F) << 12) |
           ((static_cast<unsigned char>(text[i + 2]) & 0x3F) << 6) |
           (static_cast<unsigned char>(text[i + 3]) & 0x3F);
      length = 4;
    }
    out.push_back(cp);
    i += length;
  }
  return out;
}

[[nodiscard]] bool is_subsequence(const std::vector<char32_t>& shorter,
                                  const std::vector<char32_t>& longer) {
  std::size_t i = 0;
  for (const char32_t cp : longer) {
    if (i < shorter.size() && cp == shorter[i]) {
      ++i;
    }
  }
  return i == shorter.size();
}

[[nodiscard]] MergeKind classify_merge(const std::string& a,
                                       const std::string& b) {
  if (a.empty() || b.empty()) {
    return MergeKind::kNone;
  }
  if (a == b) {
    return MergeKind::kIdentical;
  }
  const std::string* shorter = &a;
  const std::string* longer = &b;
  if (a.size() > b.size()) {
    shorter = &b;
    longer = &a;
  }
  if (longer->find(*shorter) != std::string::npos) {
    if (utf8_cp_count(*shorter) >= 2) {
      return MergeKind::kContainment;
    }
    if (longer->compare(0, shorter->size(), *shorter) == 0) {
      return MergeKind::kContainment;
    }
  }
  const auto a_cps = utf8_codepoints(a);
  const auto b_cps = utf8_codepoints(b);
  const auto& short_cps = a_cps.size() <= b_cps.size() ? a_cps : b_cps;
  const auto& long_cps = a_cps.size() <= b_cps.size() ? b_cps : a_cps;
  if (short_cps.size() >= kSimilarityMinCodePoints &&
      short_cps.size() * 100 >=
          long_cps.size() * kSimilarityMinRatioPercent &&
      is_subsequence(short_cps, long_cps)) {
    return MergeKind::kSimilar;
  }
  return MergeKind::kNone;
}

[[nodiscard]] std::vector<SubtitleEntry> merge_adjacent(
    const std::vector<SubtitleEntry>& entries, std::int32_t merge_gap_ms) {
  if (entries.empty()) {
    return {};
  }
  std::vector<SubtitleEntry> result;
  result.push_back(entries.front());
  for (std::size_t i = 1; i < entries.size(); ++i) {
    const auto& current = entries[i];
    const auto normalized = normalize_for_merge(current.text);
    if (normalized.empty()) {
      result.push_back(current);
      continue;
    }
    int last_ne_idx = -1;
    for (int j = static_cast<int>(result.size()) - 1; j >= 0; --j) {
      if (!normalize_for_merge(result[static_cast<std::size_t>(j)].text).empty()) {
        last_ne_idx = j;
        break;
      }
    }
    if (last_ne_idx < 0) {
      result.push_back(current);
      continue;
    }
    const auto last = result[static_cast<std::size_t>(last_ne_idx)];
    const auto last_norm = normalize_for_merge(last.text);
    const auto gap = current.start_ms - last.end_ms;
    const auto kind = classify_merge(last_norm, normalized);
    const std::int64_t max_gap =
        kind == MergeKind::kSimilar
            ? std::min<std::int64_t>(static_cast<std::int64_t>(merge_gap_ms),
                                     kSimilarityMaxGapMs)
            : static_cast<std::int64_t>(merge_gap_ms);
    if (gap <= max_gap && kind != MergeKind::kNone) {
      std::string merged_text;
      switch (kind) {
        case MergeKind::kContainment:
          merged_text =
              normalized.size() > last_norm.size() ? current.text : last.text;
          break;
        case MergeKind::kSimilar:
          // Inserted OCR artifacts ("Y", "VF") make the longer variant; keep
          // the shorter, artifact-free text.
          merged_text =
              normalized.size() < last_norm.size() ? current.text : last.text;
          break;
        case MergeKind::kIdentical:
        case MergeKind::kNone:
        default:
          merged_text = last.text;
          break;
      }
      result.resize(static_cast<std::size_t>(last_ne_idx) + 1);
      result[static_cast<std::size_t>(last_ne_idx)] = SubtitleEntry{
          .start_ms = last.start_ms,
          .end_ms = current.end_ms,
          .text = std::move(merged_text),
          .confidence = std::max(last.confidence, current.confidence),
      };
    } else {
      result.push_back(current);
    }
  }
  return result;
}

[[nodiscard]] std::vector<SubtitleEntry> filter_short(
    const std::vector<SubtitleEntry>& entries, std::int32_t min_duration_ms) {
  std::vector<SubtitleEntry> out;
  for (const auto& e : entries) {
    if (e.end_ms - e.start_ms >= min_duration_ms) {
      out.push_back(e);
    }
  }
  return out;
}

[[nodiscard]] bool is_strip_empty(const std::string& text) {
  // Python: not text.strip()
  std::size_t i = 0;
  while (i < text.size()) {
    const std::size_t before = i;
    if (is_python_space_byte_or_utf8(text, i)) {
      continue;
    }
    if (i == before) {
      return false;  // non-space content
    }
  }
  return true;
}

[[nodiscard]] std::vector<SubtitleEntry> filter_empty(
    const std::vector<SubtitleEntry>& entries) {
  std::vector<SubtitleEntry> out;
  for (const auto& e : entries) {
    if (!is_strip_empty(e.text)) {
      out.push_back(e);
    }
  }
  return out;
}

// ---- Phase 21 D03: entry-level persistent-watermark suppression ------------
//
// Thresholds are frozen by docs/design/subtitle-band-detection.md + phase21
// D03 acceptance. Kept as named constants so the rule is explainable and a
// single place to tune after an A/B run.

/// A watermark family needs at least this many raw binder entries (single
/// noisy lines are never suppressed).
constexpr std::size_t kWatermarkMinFamilyCount = 5;
/// The family's union duration must cover at least this % of the video.
constexpr std::int64_t kWatermarkMinCoveragePercent = 8;
/// The family's first->last span must cover at least this % of the video.
constexpr std::int64_t kWatermarkMinSpanPercent = 60;
/// "Short text" candidate bound (whitespace-stripped UTF-8 code points).
constexpr std::size_t kWatermarkShortTextMaxCodePoints = 12;
/// Candidate-subset bound for the O(m^2) family grouping; above it we fail
/// safe (return input) rather than spend unbounded time in finalize.
constexpr std::size_t kWatermarkMaxCandidates = 4096;

struct DisjointSet {
  std::vector<std::size_t> parent;

  explicit DisjointSet(std::size_t n) : parent(n) {
    for (std::size_t i = 0; i < n; ++i) {
      parent[i] = i;
    }
  }

  [[nodiscard]] std::size_t find(std::size_t x) {
    while (parent[x] != x) {
      parent[x] = parent[parent[x]];
      x = parent[x];
    }
    return x;
  }

  void unite(std::size_t a, std::size_t b) {
    a = find(a);
    b = find(b);
    if (a != b) {
      parent[b] = a;
    }
  }
};

/// Candidate = non-empty entry whose stripped text is URL/domain-like or short.
[[nodiscard]] bool is_watermark_candidate(const SubtitleEntry& entry) {
  if (is_strip_empty(entry.text)) {
    return false;
  }
  const std::string normalized = normalize_for_merge(entry.text);
  if (normalized.empty()) {
    return false;
  }
  if (is_watermark_text(normalized)) {
    return true;
  }
  return utf8_cp_count(normalized) <= kWatermarkShortTextMaxCodePoints;
}

/// Same-overlay predicate: exact text, existing merge containment/subsequence,
/// or bounded edit similarity (one-character OCR variants of the same overlay).
[[nodiscard]] bool same_watermark_family(const std::string& a,
                                         const std::string& b) {
  if (a == b) {
    return true;
  }
  if (classify_merge(a, b) != MergeKind::kNone) {
    return true;
  }
  const std::size_t shorter = std::min(utf8_cp_count(a), utf8_cp_count(b));
  const std::size_t longer = std::max(utf8_cp_count(a), utf8_cp_count(b));
  // is_similar's edit-ratio bound (0.4, or 0.34 up to 4 code points) implies
  // shorter/longer >= 0.66; reject impossible pairs before the O(len^2) DP so
  // the O(m^2) candidate comparison stays cheap.
  if (longer == 0 || shorter * 100 < longer * 66) {
    return false;
  }
  return is_similar(a, b);
}

struct WatermarkFamily {
  std::vector<std::size_t> members;  // indices into the candidate subset
  std::vector<std::pair<std::int64_t, std::int64_t>> intervals;
  std::int64_t min_start{0};
  std::int64_t max_end{0};
};

/// Safety guard: a removable family must carry a watermark signal - at least
/// one URL/domain/mixed-token text, or at least two distinct OCR variants of
/// the same text. Exact-identical repeated normal dialogue (looped source,
/// repeated lyric/short line whose total coverage happens to pass the frozen
/// 8%/60% thresholds) is kept: such a family has no URL evidence and only one
/// variant. See the phase21 128s loop A/B (40-entry baseline).
[[nodiscard]] bool family_has_watermark_signal(
    const WatermarkFamily& family, const std::vector<SubtitleEntry>& entries) {
  std::vector<std::string> variants;
  variants.reserve(family.members.size());
  for (const std::size_t index : family.members) {
    const std::string normalized = normalize_for_merge(entries[index].text);
    if (is_watermark_text(normalized)) {
      return true;
    }
    variants.push_back(normalized);
  }
  std::sort(variants.begin(), variants.end());
  variants.erase(std::unique(variants.begin(), variants.end()), variants.end());
  return variants.size() >= 2;
}

/// Remove a whole watermark family only when all three frozen conditions hold.
[[nodiscard]] bool family_is_persistent_watermark(
    const WatermarkFamily& family, std::int64_t video_duration_ms) {
  if (family.members.size() < kWatermarkMinFamilyCount) {
    return false;
  }
  // Coverage: union length of the member intervals (overlaps counted once).
  std::vector<std::pair<std::int64_t, std::int64_t>> intervals = family.intervals;
  std::sort(intervals.begin(), intervals.end());
  std::int64_t coverage = 0;
  bool open = false;
  std::int64_t union_start = 0;
  std::int64_t union_end = 0;
  for (const auto& [start, end] : intervals) {
    if (end <= start) {
      continue;
    }
    if (!open) {
      union_start = start;
      union_end = end;
      open = true;
    } else if (start <= union_end) {
      union_end = std::max(union_end, end);
    } else {
      coverage += union_end - union_start;
      union_start = start;
      union_end = end;
    }
  }
  if (open) {
    coverage += union_end - union_start;
  }
  const std::int64_t span = family.max_end - family.min_start;
  return coverage * 100 >= video_duration_ms * kWatermarkMinCoveragePercent &&
         span * 100 >= video_duration_ms * kWatermarkMinSpanPercent;
}

}  // namespace

std::vector<SubtitleEntry> merge_entries(const std::vector<SubtitleEntry>& entries,
                                         std::int32_t merge_gap_ms,
                                         std::int32_t min_duration_ms,
                                         bool drop_empty_text) {
  if (entries.empty()) {
    return {};
  }
  auto merged = merge_adjacent(entries, merge_gap_ms);
  auto after_empty =
      drop_empty_text ? filter_empty(merged) : std::move(merged);
  auto filtered_short = filter_short(after_empty, min_duration_ms);
  return merge_adjacent(filtered_short, merge_gap_ms);
}

std::vector<SubtitleEntry> suppress_watermark_entries(
    const std::vector<SubtitleEntry>& entries,
    std::int64_t video_duration_ms) {
  if (video_duration_ms <= 0 ||
      entries.size() < kWatermarkMinFamilyCount) {
    return entries;
  }

  // 1) Candidate subset: only "short text or URLish" entries take part in the
  //    O(m^2) grouping. Empty entries are deliberately excluded: blank cues are
  //    merge inputs for adjacent text and must keep their exact behaviour.
  std::vector<std::size_t> candidates;
  std::vector<std::string> keys;
  candidates.reserve(entries.size());
  keys.reserve(entries.size());
  for (std::size_t i = 0; i < entries.size(); ++i) {
    if (!is_watermark_candidate(entries[i])) {
      continue;
    }
    candidates.push_back(i);
    keys.push_back(normalize_for_merge(entries[i].text));
  }
  if (candidates.size() < kWatermarkMinFamilyCount) {
    return entries;
  }
  if (candidates.size() > kWatermarkMaxCandidates) {
    return entries;  // fail safe: bound the O(m^2) comparison
  }

  // 2) Union-find family grouping over the candidate subset only.
  DisjointSet dsu(candidates.size());
  for (std::size_t a = 0; a < candidates.size(); ++a) {
    for (std::size_t b = a + 1; b < candidates.size(); ++b) {
      if (dsu.find(a) == dsu.find(b)) {
        continue;
      }
      if (same_watermark_family(keys[a], keys[b])) {
        dsu.unite(a, b);
      }
    }
  }

  // 3) Aggregate per-family count / coverage / span.
  std::vector<WatermarkFamily> families(candidates.size());
  for (std::size_t i = 0; i < candidates.size(); ++i) {
    const auto& entry = entries[candidates[i]];
    const std::int64_t start = std::min(entry.start_ms, entry.end_ms);
    const std::int64_t end = std::max(entry.start_ms, entry.end_ms);
    auto& family = families[dsu.find(i)];
    if (family.members.empty()) {
      family.min_start = start;
      family.max_end = end;
    } else {
      family.min_start = std::min(family.min_start, start);
      family.max_end = std::max(family.max_end, end);
    }
    family.members.push_back(candidates[i]);
    family.intervals.emplace_back(start, end);
  }

  // 4) Mark every entry of a family that behaves like a full-film watermark.
  std::vector<bool> removed(entries.size(), false);
  for (auto& family : families) {
    if (family.members.empty() ||
        !family_has_watermark_signal(family, entries) ||
        !family_is_persistent_watermark(family, video_duration_ms)) {
      continue;
    }
    for (const std::size_t index : family.members) {
      removed[index] = true;
    }
  }

  // 5) Preserve input order; removed entries must never reach the final output.
  std::vector<SubtitleEntry> out;
  out.reserve(entries.size());
  for (std::size_t i = 0; i < entries.size(); ++i) {
    if (!removed[i]) {
      out.push_back(entries[i]);
    }
  }
  return out;
}

}  // namespace sublift
