#include "sublift/dedupe.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

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

}  // namespace sublift
