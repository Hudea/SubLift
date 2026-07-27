#include "sublift/line_select.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <regex>
#include <utility>

namespace sublift {
namespace {

// ---- UTF-8 helpers ----

[[nodiscard]] bool decode_utf8(std::string_view s, std::size_t& i,
                               char32_t& out) noexcept {
  if (i >= s.size()) {
    return false;
  }
  const auto c0 = static_cast<unsigned char>(s[i]);
  if (c0 < 0x80) {
    out = c0;
    ++i;
    return true;
  }
  if ((c0 & 0xE0) == 0xC0 && i + 1 < s.size()) {
    out = (static_cast<char32_t>(c0 & 0x1F) << 6) |
          (static_cast<unsigned char>(s[i + 1]) & 0x3F);
    i += 2;
    return true;
  }
  if ((c0 & 0xF0) == 0xE0 && i + 2 < s.size()) {
    out = (static_cast<char32_t>(c0 & 0x0F) << 12) |
          ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 6) |
          (static_cast<unsigned char>(s[i + 2]) & 0x3F);
    i += 3;
    return true;
  }
  if ((c0 & 0xF8) == 0xF0 && i + 3 < s.size()) {
    out = (static_cast<char32_t>(c0 & 0x07) << 18) |
          ((static_cast<unsigned char>(s[i + 1]) & 0x3F) << 12) |
          ((static_cast<unsigned char>(s[i + 2]) & 0x3F) << 6) |
          (static_cast<unsigned char>(s[i + 3]) & 0x3F);
    i += 4;
    return true;
  }
  // Invalid: skip one byte.
  out = c0;
  ++i;
  return true;
}

[[nodiscard]] bool is_cjk_cp(char32_t cp) noexcept {
  return (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) ||
         (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0x20000 && cp <= 0x2A6DF);
}

[[nodiscard]] bool is_latin_letter(char32_t cp) noexcept {
  return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z');
}

/// Align with Python 3 `str.isspace()` / `re \s` for common Zs + ASCII controls.
[[nodiscard]] bool is_space_cp(char32_t cp) noexcept {
  if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == '\f' ||
      cp == '\v') {
    return true;
  }
  // NBSP, Ogham, en/em/thin spaces, LS/PS, NNBSP, MMSP, ideographic space
  if (cp == 0x00A0 || cp == 0x1680 || cp == 0x2028 || cp == 0x2029 ||
      cp == 0x202F || cp == 0x205F || cp == 0x3000) {
    return true;
  }
  return cp >= 0x2000 && cp <= 0x200A;
}

[[nodiscard]] std::string collapse_ws(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  bool prev_space = false;
  std::size_t i = 0;
  while (i < text.size()) {
    char32_t cp = 0;
    const std::size_t start = i;
    if (!decode_utf8(text, i, cp)) {
      break;
    }
    if (is_space_cp(cp)) {
      if (!prev_space && !out.empty()) {
        out.push_back(' ');
        prev_space = true;
      }
    } else {
      out.append(text.substr(start, i - start));
      prev_space = false;
    }
  }
  // strip
  while (!out.empty() && out.front() == ' ') {
    out.erase(out.begin());
  }
  while (!out.empty() && out.back() == ' ') {
    out.pop_back();
  }
  return out;
}

// Python _CJK_EDGE_CLASS is BMP-only (no Extension B) for attached-latin regex.
[[nodiscard]] bool is_cjk_edge_bmp(char32_t cp) noexcept {
  return (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) ||
         (cp >= 0xF900 && cp <= 0xFAFF);
}

[[nodiscard]] bool is_cjk_edge_open(char32_t cp) noexcept {
  return is_cjk_edge_bmp(cp) || cp == 0xFF08 /*（*/ || cp == 0x3010 /*【*/ ||
         cp == 0x300A /*《*/ || cp == 0x300C /*「*/ || cp == 0x300E /*『*/ ||
         cp == 0x201C /*“*/;
}

[[nodiscard]] bool is_cjk_edge_close(char32_t cp) noexcept {
  return is_cjk_edge_bmp(cp) || cp == 0xFF09 /*）*/ || cp == 0x3011 /*】*/ ||
         cp == 0x300B /*》*/ || cp == 0x300D /*」*/ || cp == 0x300F /*』*/ ||
         cp == 0x201D /*”*/;
}

[[nodiscard]] bool is_leading_attached_latin_char(char32_t cp) noexcept {
  // [A-Za-z0-9._/\-]
  return is_latin_letter(cp) || (cp >= '0' && cp <= '9') || cp == '.' ||
         cp == '_' || cp == '/' || cp == '-';
}

[[nodiscard]] bool is_trailing_latin_body(char32_t cp) noexcept {
  // [A-Za-z0-9 .:_/\-]
  return is_latin_letter(cp) || (cp >= '0' && cp <= '9') || cp == ' ' ||
         cp == '.' || cp == ':' || cp == '_' || cp == '/' || cp == '-';
}

[[nodiscard]] bool is_trailing_cjk_punct(char32_t cp) noexcept {
  // [、，。！？…]
  return cp == 0x3001 || cp == 0xFF0C || cp == 0x3002 || cp == 0xFF01 ||
         cp == 0xFF1F || cp == 0x2026;
}

/// ^[A-Za-z0-9._/\-]+(?=CJK_EDGE_OPEN) — strip once.
[[nodiscard]] std::string strip_leading_attached_latin(std::string t) {
  std::size_t i = 0;
  std::size_t latin_end = 0;
  bool saw = false;
  while (i < t.size()) {
    char32_t cp = 0;
    if (!decode_utf8(t, i, cp)) {
      break;
    }
    if (!is_leading_attached_latin_char(cp)) {
      break;
    }
    saw = true;
    latin_end = i;
  }
  if (!saw) {
    return t;
  }
  // Lookahead must be CJK edge open.
  if (latin_end >= t.size()) {
    return t;
  }
  std::size_t j = latin_end;
  char32_t next = 0;
  if (!decode_utf8(t, j, next) || !is_cjk_edge_open(next)) {
    return t;
  }
  return t.substr(latin_end);
}

/// (?<=CJK_EDGE_CLOSE)[A-Za-z][A-Za-z0-9 .:_/\-]*[、，。！？…]*$
[[nodiscard]] std::string strip_trailing_attached_latin(std::string t) {
  if (t.empty()) {
    return t;
  }
  struct CP {
    char32_t cp;
    std::size_t begin;
  };
  std::vector<CP> cps;
  std::size_t i = 0;
  while (i < t.size()) {
    char32_t cp = 0;
    const std::size_t b = i;
    if (!decode_utf8(t, i, cp)) {
      break;
    }
    cps.push_back(CP{cp, b});
  }
  if (cps.size() < 2) {
    return t;
  }
  // Match from the end: optional punct*, then latin body ending with letter at start.
  std::size_t end = cps.size();  // exclusive
  while (end > 0 && is_trailing_cjk_punct(cps[end - 1].cp)) {
    --end;
  }
  if (end == 0) {
    return t;
  }
  std::size_t start = end;
  while (start > 0 && is_trailing_latin_body(cps[start - 1].cp)) {
    --start;
  }
  if (start >= end) {
    return t;
  }
  // First of latin run must be [A-Za-z]
  if (!is_latin_letter(cps[start].cp)) {
    return t;
  }
  // Lookbehind: codepoint before start must be CJK edge close
  if (start == 0 || !is_cjk_edge_close(cps[start - 1].cp)) {
    return t;
  }
  // Match extends through optional punct to string end (already required by $).
  // If we stripped punct from end above, include them in the match by using
  // full string length from start.
  return t.substr(0, cps[start].begin);
}

[[nodiscard]] std::string replace_all(std::string s, std::string_view from,
                                      std::string_view to) {
  if (from.empty()) {
    return s;
  }
  std::size_t pos = 0;
  while ((pos = s.find(from, pos)) != std::string::npos) {
    s.replace(pos, from.size(), to);
    pos += to.size();
  }
  return s;
}

[[nodiscard]] std::string consensus_key(std::string_view text,
                                        std::string_view script) {
  const auto normalized = normalize_ocr_text(text);
  if (script != SCRIPT_CJK) {
    return normalized;
  }
  std::string cjk_only;
  std::size_t i = 0;
  while (i < normalized.size()) {
    char32_t cp = 0;
    const std::size_t b = i;
    if (!decode_utf8(normalized, i, cp)) {
      break;
    }
    if (is_cjk_cp(cp)) {
      cjk_only.append(normalized.substr(b, i - b));
    }
  }
  return cjk_only.empty() ? normalized : cjk_only;
}

const std::regex kLeadingLatinEdge{R"(^[A-Za-z][A-Za-z0-9 .:_/\-]*)"};
const std::regex kTrailingLatinEdge{R"([A-Za-z][A-Za-z0-9 .:_/\-]*$)"};

[[nodiscard]] std::string edge_match(const std::string& text,
                                     const std::regex& pattern) {
  std::smatch m;
  if (std::regex_search(text, m, pattern)) {
    auto s = m.str(0);
    // strip
    while (!s.empty() && s.front() == ' ') {
      s.erase(s.begin());
    }
    while (!s.empty() && s.back() == ' ') {
      s.pop_back();
    }
    return s;
  }
  return {};
}

[[nodiscard]] std::string trim_unstable_latin_edges(
    std::string text, const std::vector<std::string>& cluster_texts) {
  auto leading = edge_match(text, kLeadingLatinEdge);
  if (!leading.empty()) {
    std::vector<std::string> variants;
    for (const auto& c : cluster_texts) {
      variants.push_back(edge_match(c, kLeadingLatinEdge));
    }
    std::sort(variants.begin(), variants.end());
    variants.erase(std::unique(variants.begin(), variants.end()), variants.end());
    if (variants.size() > 1) {
      text = std::regex_replace(text, kLeadingLatinEdge, "",
                                std::regex_constants::format_first_only);
      text = collapse_ws(text);
    }
  }
  auto trailing = edge_match(text, kTrailingLatinEdge);
  if (!trailing.empty()) {
    std::vector<std::string> variants;
    for (const auto& c : cluster_texts) {
      variants.push_back(edge_match(c, kTrailingLatinEdge));
    }
    std::sort(variants.begin(), variants.end());
    variants.erase(std::unique(variants.begin(), variants.end()), variants.end());
    if (variants.size() > 1) {
      // replace last match only — std::regex_replace all; simulate by reverse
      std::smatch m;
      if (std::regex_search(text, m, kTrailingLatinEdge)) {
        text.erase(static_cast<std::size_t>(m.position()),
                   static_cast<std::size_t>(m.length()));
        text = collapse_ws(text);
      }
    }
  }
  return text;
}

}  // namespace

std::string normalize_ocr_text(std::string_view text) {
  return collapse_ws(text);
}

std::string cleanup_subtitle_text(std::string_view text, std::string_view script) {
  auto t = normalize_ocr_text(text);
  if (t.empty()) {
    return {};
  }
  if (script == SCRIPT_CJK && cjk_ratio(t) > 0.0) {
    t = strip_leading_attached_latin(std::move(t));
    t = strip_trailing_attached_latin(std::move(t));
  }
  t = replace_all(std::move(t), "⋯", "…");
  t = replace_all(std::move(t), "...", "…");
  t = replace_all(std::move(t), "『", "「");
  t = replace_all(std::move(t), "』", "」");
  t = replace_all(std::move(t), "－", "-");
  t = replace_all(std::move(t), "—", "-");
  if (script == SCRIPT_CJK && !t.empty() && t.back() == '.' &&
      cjk_ratio(t.substr(0, t.size() - 1)) >= 0.5) {
    t.pop_back();
    t += "…";
  }
  return normalize_ocr_text(t);
}

double cjk_ratio(std::string_view text) {
  int total = 0;
  int cjk = 0;
  std::size_t i = 0;
  while (i < text.size()) {
    char32_t cp = 0;
    if (!decode_utf8(text, i, cp)) {
      break;
    }
    if (is_space_cp(cp)) {
      continue;
    }
    ++total;
    if (is_cjk_cp(cp)) {
      ++cjk;
    }
  }
  if (total == 0) {
    return 0.0;
  }
  return static_cast<double>(cjk) / static_cast<double>(total);
}

double latin_ratio(std::string_view text) {
  int total = 0;
  int lat = 0;
  std::size_t i = 0;
  while (i < text.size()) {
    char32_t cp = 0;
    if (!decode_utf8(text, i, cp)) {
      break;
    }
    if (is_space_cp(cp)) {
      continue;
    }
    ++total;
    if (is_latin_letter(cp)) {
      ++lat;
    }
  }
  if (total == 0) {
    return 0.0;
  }
  return static_cast<double>(lat) / static_cast<double>(total);
}

double script_score(std::string_view text, std::string_view script) {
  if (script == SCRIPT_AUTO) {
    return 0.5 + 0.5 * std::max(cjk_ratio(text), latin_ratio(text));
  }
  if (script == SCRIPT_CJK) {
    return cjk_ratio(text);
  }
  if (script == SCRIPT_LATIN) {
    return latin_ratio(text);
  }
  return 0.0;
}

LineScore score_line(const OcrLine& line, const SubtitleProfile& profile) {
  const double s_script = script_score(line.text, profile.script);
  const double cy =
      static_cast<double>(line.box.y) + static_cast<double>(line.box.height) / 2.0;
  const double band_lo = static_cast<double>(profile.y_min);
  double band_hi = static_cast<double>(profile.y_max);
  if (!(profile.y_max > profile.y_min)) {
    band_hi = band_lo + 1.0;
  }
  const double band_h = std::max(1.0, band_hi - band_lo);
  double s_y = 0.0;
  if (band_lo <= cy && cy <= band_hi) {
    const double dist =
        std::abs(cy - static_cast<double>(profile.center_y)) / band_h;
    s_y = std::max(0.0, 1.0 - dist);
  } else {
    const double overflow =
        (cy < band_lo) ? (band_lo - cy) / band_h : (cy - band_hi) / band_h;
    s_y = std::max(0.0, 1.0 - overflow);
  }
  const double ph = std::max(1.0, static_cast<double>(profile.height));
  const double lh = std::max(1.0, static_cast<double>(line.box.height));
  const double ratio = std::min(lh, ph) / std::max(lh, ph);
  const double s_h = ratio;
  const double cx =
      static_cast<double>(line.box.x) + static_cast<double>(line.box.width) / 2.0;
  const double span =
      std::max(static_cast<double>(profile.center_x) * 2.0, 1.0);
  const double s_c =
      std::max(0.0, 1.0 - std::abs(cx - static_cast<double>(profile.center_x)) / span);
  const double conf = std::max(0.0, std::min(1.0, line.confidence));
  const double total = 0.40 * s_script + 0.30 * s_y + 0.15 * s_h + 0.10 * s_c +
                       0.05 * conf;
  return LineScore{total, s_script, s_y, s_h, s_c, conf};
}

std::optional<OcrLine> select_line(const std::vector<OcrLine>& lines,
                                   const SubtitleProfile& profile,
                                   double min_score, double min_script) {
  if (lines.empty()) {
    return std::nullopt;
  }
  std::optional<OcrLine> best;
  double best_score = -1.0;
  for (const auto& line : lines) {
    // Python: if not line.text.strip(): continue (Unicode whitespace)
    if (normalize_ocr_text(line.text).empty()) {
      continue;
    }
    const auto sc = score_line(line, profile);
    if (sc.total < min_score) {
      continue;
    }
    if (profile.script != SCRIPT_AUTO && sc.script < min_script) {
      continue;
    }
    if (sc.total > best_score) {
      best_score = sc.total;
      best = line;
    }
  }
  return best;
}

[[nodiscard]] std::vector<char32_t> to_codepoints(std::string_view s) {
  std::vector<char32_t> out;
  std::size_t i = 0;
  while (i < s.size()) {
    char32_t cp = 0;
    if (!decode_utf8(s, i, cp)) {
      break;
    }
    out.push_back(cp);
  }
  return out;
}

int edit_distance(std::string_view a, std::string_view b) {
  // Python operates on Unicode code points, not UTF-8 bytes.
  const auto ca = to_codepoints(a);
  const auto cb = to_codepoints(b);
  if (ca == cb) {
    return 0;
  }
  if (ca.empty()) {
    return static_cast<int>(cb.size());
  }
  if (cb.empty()) {
    return static_cast<int>(ca.size());
  }
  std::vector<int> prev(cb.size() + 1);
  for (std::size_t j = 0; j <= cb.size(); ++j) {
    prev[j] = static_cast<int>(j);
  }
  for (std::size_t i = 1; i <= ca.size(); ++i) {
    std::vector<int> cur(cb.size() + 1);
    cur[0] = static_cast<int>(i);
    for (std::size_t j = 1; j <= cb.size(); ++j) {
      const int ins = cur[j - 1] + 1;
      const int del = prev[j] + 1;
      const int sub = prev[j - 1] + (ca[i - 1] == cb[j - 1] ? 0 : 1);
      cur[j] = std::min({ins, del, sub});
    }
    prev = std::move(cur);
  }
  return prev[cb.size()];
}

bool is_similar(std::string_view a, std::string_view b) {
  if (a.empty() || b.empty()) {
    return a == b;
  }
  // Python: (a in b or b in a) on Unicode strings.
  const auto ca = to_codepoints(a);
  const auto cb = to_codepoints(b);
  auto contains = [](const std::vector<char32_t>& hay,
                     const std::vector<char32_t>& needle) {
    if (needle.empty() || needle.size() > hay.size()) {
      return false;
    }
    for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
      if (std::equal(needle.begin(), needle.end(), hay.begin() +
                                                       static_cast<std::ptrdiff_t>(i))) {
        return true;
      }
    }
    return false;
  };
  if ((contains(ca, cb) || contains(cb, ca)) &&
      std::min(ca.size(), cb.size()) >= 4) {
    return true;
  }
  const int ed = edit_distance(a, b);
  const auto mx = static_cast<double>(std::max(ca.size(), cb.size()));
  const double threshold = (std::max(ca.size(), cb.size()) <= 4) ? 0.34 : 0.4;
  return static_cast<double>(ed) / mx <= threshold;
}

ConsensusResult consensus_text(
    const std::vector<std::pair<std::string, double>>& samples,
    std::string_view script) {
  struct Sample {
    int index;
    std::string text;
    std::string key;
    double conf;
  };
  std::vector<Sample> cleaned;
  for (std::size_t index = 0; index < samples.size(); ++index) {
    const auto norm = normalize_ocr_text(samples[index].first);
    if (norm.empty()) {
      continue;
    }
    cleaned.push_back(Sample{static_cast<int>(index), norm,
                             consensus_key(norm, script), samples[index].second});
  }
  if (cleaned.empty()) {
    return ConsensusResult{"", 0.0, 0};
  }

  std::vector<std::vector<Sample>> neighborhoods;
  for (const auto& center : cleaned) {
    std::vector<Sample> neighborhood;
    for (const auto& sample : cleaned) {
      if (is_similar(sample.key, center.key)) {
        neighborhood.push_back(sample);
      }
    }
    neighborhoods.push_back(std::move(neighborhood));
  }

  auto cluster_rank = [](const std::vector<Sample>& cluster) {
    int total_cost = 0;
    for (const auto& left : cluster) {
      for (const auto& right : cluster) {
        total_cost += edit_distance(left.key, right.key);
      }
    }
    int min_idx = cluster.front().index;
    for (const auto& item : cluster) {
      min_idx = std::min(min_idx, item.index);
    }
    return std::make_tuple(-static_cast<int>(cluster.size()), total_cost, min_idx);
  };

  auto best_cluster = *std::min_element(
      neighborhoods.begin(), neighborhoods.end(),
      [&](const auto& a, const auto& b) { return cluster_rank(a) < cluster_rank(b); });
  const int votes = static_cast<int>(best_cluster.size());

  auto medoid = *std::min_element(
      best_cluster.begin(), best_cluster.end(),
      [&](const Sample& cand, const Sample& other) {
        int cost_key = 0;
        int cost_text = 0;
        int cost_key_o = 0;
        int cost_text_o = 0;
        for (const auto& x : best_cluster) {
          cost_key += edit_distance(cand.key, x.key);
          cost_text += edit_distance(cand.text, x.text);
          cost_key_o += edit_distance(other.key, x.key);
          cost_text_o += edit_distance(other.text, x.text);
        }
        return std::make_tuple(cost_key, cost_text, cand.index) <
               std::make_tuple(cost_key_o, cost_text_o, other.index);
      });

  std::string text = medoid.text;
  std::vector<std::string> cluster_texts;
  for (const auto& item : best_cluster) {
    cluster_texts.push_back(item.text);
  }
  std::vector<std::string> unique = cluster_texts;
  std::sort(unique.begin(), unique.end());
  unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
  if (script == SCRIPT_CJK && unique.size() > 1) {
    text = trim_unstable_latin_edges(text, cluster_texts);
  }
  double confidence = 0.0;
  for (const auto& item : best_cluster) {
    confidence += item.conf;
  }
  confidence /= static_cast<double>(votes);
  return ConsensusResult{text, confidence, votes};
}

bool should_accept_text(std::string_view text, double confidence,
                        const SubtitleProfile& profile,
                        double confidence_threshold, double low_conf_threshold,
                        int support_votes, int min_stable_votes) {
  if (text.find_first_not_of(" \t\n\r\f\v") == std::string_view::npos) {
    return false;
  }
  const double s = script_score(text, profile.script);
  if (profile.script == SCRIPT_CJK && s < 0.08 && latin_ratio(text) > 0.5) {
    return false;
  }
  if (profile.script == SCRIPT_LATIN && s < 0.08 && cjk_ratio(text) > 0.5) {
    return false;
  }
  if (confidence >= confidence_threshold &&
      (profile.script == SCRIPT_AUTO || s >= 0.08)) {
    return true;
  }
  if (confidence >= low_conf_threshold && support_votes >= min_stable_votes &&
      s >= 0.20) {
    return true;
  }
  if (confidence >= std::max(low_conf_threshold, 0.35) && s >= 0.45 &&
      support_votes >= 1) {
    return true;
  }
  return confidence >= confidence_threshold && s >= 0.35;
}

}  // namespace sublift
