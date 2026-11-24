#include <sublift/text/dedupe.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>
#include <vector>

#include <sublift/text/cleaning.hpp>

namespace sublift::text {
namespace {

[[nodiscard]] constexpr std::int64_t safe_sub(std::int64_t a, std::int64_t b) noexcept {
#if defined(__GNUC__) || defined(__clang__)
  std::int64_t result = 0;
  if (__builtin_sub_overflow(a, b, &result)) {
    return (a > b) ? std::numeric_limits<std::int64_t>::max()
                   : std::numeric_limits<std::int64_t>::min();
  }
  return result;
#else
  constexpr auto kMax = std::numeric_limits<std::int64_t>::max();
  constexpr auto kMin = std::numeric_limits<std::int64_t>::min();
  if (b < 0 && a > kMax + b) return kMax;
  if (b > 0 && a < kMin + b) return kMin;
  return a - b;
#endif
}

constexpr std::size_t kSimilarityMinCodePoints = 4;
constexpr std::size_t kSimilarityMinRatioPercent = 60;
constexpr std::int64_t kSimilarityMaxGapMs = 300;

enum class MergeKind { kNone, kIdentical, kContainment, kSimilar };

[[nodiscard]] std::string normalize_for_merge(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  std::size_t i = 0;
  while (i < text.size()) {
    char32_t cp = 0;
    const std::size_t start = i;
    if (!decode_utf8(text, i, cp)) {
      break;
    }
    if (!is_space_cp(cp)) {
      out.append(text.substr(start, i - start));
    }
  }
  return out;
}

[[nodiscard]] std::size_t utf8_cp_count(std::string_view text) {
  std::size_t count = 0;
  std::size_t i = 0;
  char32_t cp = 0;
  while (decode_utf8(text, i, cp)) {
    ++count;
  }
  return count;
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
  const auto a_cps = to_codepoints(a);
  const auto b_cps = to_codepoints(b);
  const auto& short_cps = a_cps.size() <= b_cps.size() ? a_cps : b_cps;
  const auto& long_cps = a_cps.size() <= b_cps.size() ? b_cps : a_cps;
  if (short_cps.size() >= kSimilarityMinCodePoints &&
      short_cps.size() * 100 >= long_cps.size() * kSimilarityMinRatioPercent &&
      is_subsequence(short_cps, long_cps)) {
    return MergeKind::kSimilar;
  }
  return MergeKind::kNone;
}

[[nodiscard]] std::vector<SubtitleEntry> merge_adjacent(
    const std::vector<SubtitleEntry>& entries, std::int64_t merge_gap_ms) {
  if (entries.empty()) {
    return {};
  }
  std::vector<SubtitleEntry> result;
  result.reserve(entries.size());
  result.push_back(entries.front());

  for (std::size_t i = 1; i < entries.size(); ++i) {
    const auto& current = entries[i];
    const auto normalized = normalize_for_merge(current.text);
    if (normalized.empty()) {
      result.push_back(current);
      continue;
    }

    std::ptrdiff_t last_ne_idx = -1;
    for (auto j = static_cast<std::ptrdiff_t>(result.size()) - 1; j >= 0; --j) {
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
    const auto gap = safe_sub(current.range.start_ms, last.range.end_ms);
    const auto kind = classify_merge(last_norm, normalized);
    const std::int64_t max_gap =
        (kind == MergeKind::kSimilar)
            ? std::min<std::int64_t>(merge_gap_ms, kSimilarityMaxGapMs)
            : merge_gap_ms;

    if (gap <= max_gap && kind != MergeKind::kNone) {
      std::string merged_text;
      switch (kind) {
        case MergeKind::kContainment:
          merged_text =
              (normalized.size() > last_norm.size()) ? current.text : last.text;
          break;
        case MergeKind::kSimilar:
          // Inserted OCR noise makes the longer text; keep the shorter clean text.
          merged_text =
              (normalized.size() < last_norm.size()) ? current.text : last.text;
          break;
        case MergeKind::kIdentical:
        case MergeKind::kNone:
        default:
          merged_text = last.text;
          break;
      }
      result.resize(static_cast<std::size_t>(last_ne_idx) + 1);
      result[static_cast<std::size_t>(last_ne_idx)] = SubtitleEntry{
          .id = last.id,
          .range = TimeRange{last.range.start_ms, current.range.end_ms},
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
    const std::vector<SubtitleEntry>& entries, std::int64_t min_duration_ms) {
  std::vector<SubtitleEntry> out;
  out.reserve(entries.size());
  for (const auto& e : entries) {
    if (safe_sub(e.range.end_ms, e.range.start_ms) >= min_duration_ms) {
      out.push_back(e);
    }
  }
  return out;
}

[[nodiscard]] bool is_strip_empty(std::string_view text) {
  return normalize_for_merge(text).empty();
}

[[nodiscard]] std::vector<SubtitleEntry> filter_empty(
    const std::vector<SubtitleEntry>& entries) {
  std::vector<SubtitleEntry> out;
  out.reserve(entries.size());
  for (const auto& e : entries) {
    if (!is_strip_empty(e.text)) {
      out.push_back(e);
    }
  }
  return out;
}

} // namespace

std::vector<SubtitleEntry> merge_entries(
    const std::vector<SubtitleEntry>& entries,
    std::int64_t merge_gap_ms,
    std::int64_t min_duration_ms,
    bool drop_empty_text) {
  if (entries.empty()) {
    return {};
  }
  // Pass 1: Adjacent merge
  auto merged = merge_adjacent(entries, merge_gap_ms);

  // Pass 2: Filter empty (controlled by drop_empty_text)
  auto after_empty = drop_empty_text ? filter_empty(merged) : std::move(merged);

  // Pass 3: Filter short segments
  auto filtered_short = filter_short(after_empty, min_duration_ms);

  // Pass 4: Secondary adjacent merge to fuse newly adjacent matching segments
  auto final_result = merge_adjacent(filtered_short, merge_gap_ms);

  // Assign sequential 1-based IDs
  for (std::size_t i = 0; i < final_result.size(); ++i) {
    final_result[i].id = static_cast<std::uint64_t>(i + 1);
  }

  return final_result;
}

} // namespace sublift::text
