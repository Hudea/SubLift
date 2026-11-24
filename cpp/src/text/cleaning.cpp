#include <sublift/text/cleaning.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sublift::text {
namespace {

[[nodiscard]] bool is_cjk_edge_bmp(char32_t cp) noexcept {
  return (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) ||
         (cp >= 0xF900 && cp <= 0xFAFF);
}

[[nodiscard]] bool is_cjk_edge_open(char32_t cp) noexcept {
  return is_cjk_edge_bmp(cp) || cp == 0xFF08 /*（*/ || cp == 0x3010 /*【*/ ||
         cp == 0x300A /*《*/ || cp == 0x300C /*「*/ || cp == 0x300E /*『*/ ||
         cp == 0x201C /*“*/ || cp == '(' || cp == '[';
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
  std::size_t end = cps.size();
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

} // namespace

bool decode_utf8(std::string_view s, std::size_t& i, char32_t& out) noexcept {
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
  // Invalid byte fallback
  out = c0;
  ++i;
  return true;
}

bool is_cjk_cp(char32_t cp) noexcept {
  return (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) ||
         (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0x20000 && cp <= 0x2A6DF);
}

bool is_latin_letter(char32_t cp) noexcept {
  return (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z');
}

bool is_space_cp(char32_t cp) noexcept {
  if (cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == '\f' ||
      cp == '\v') {
    return true;
  }
  if (cp == 0x00A0 || cp == 0x1680 || cp == 0x2028 || cp == 0x2029 ||
      cp == 0x202F || cp == 0x205F || cp == 0x3000) {
    return true;
  }
  return cp >= 0x2000 && cp <= 0x200A;
}

std::vector<char32_t> to_codepoints(std::string_view s) {
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

std::string normalize_whitespace(std::string_view text) {
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
  // Trim edges
  while (!out.empty() && out.front() == ' ') {
    out.erase(out.begin());
  }
  while (!out.empty() && out.back() == ' ') {
    out.pop_back();
  }
  return out;
}

std::string cleanup_subtitle_text(std::string_view text, std::string_view script) {
  auto t = normalize_whitespace(text);
  if (t.empty()) {
    return {};
  }
  if ((script == SCRIPT_CJK || (script == SCRIPT_AUTO && cjk_ratio(t) > 0.0)) && cjk_ratio(t) > 0.0) {
    t = strip_leading_attached_latin(std::move(t));
    t = strip_trailing_attached_latin(std::move(t));
  }
  t = replace_all(std::move(t), "⋯", "…");
  t = replace_all(std::move(t), "...", "…");
  t = replace_all(std::move(t), "『", "「");
  t = replace_all(std::move(t), "』", "」");
  t = replace_all(std::move(t), "－", "-");
  t = replace_all(std::move(t), "—", "-");
  if ((script == SCRIPT_CJK || script == SCRIPT_AUTO) && !t.empty() && t.back() == '.' &&
      cjk_ratio(t.substr(0, t.size() - 1)) >= 0.5) {
    t.pop_back();
    t += "…";
  }
  // Strip trailing orphan punctuation noise
  while (!t.empty() && (t.back() == '_' || t.back() == '|' || t.back() == '~')) {
    t.pop_back();
  }
  return normalize_whitespace(t);
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

} // namespace sublift::text
