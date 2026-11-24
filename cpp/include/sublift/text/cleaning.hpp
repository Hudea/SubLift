#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include <sublift/core/models.hpp>

namespace sublift::text {

/// UTF-8 and Unicode character inspection helpers
[[nodiscard]] bool decode_utf8(std::string_view s, std::size_t& i, char32_t& out) noexcept;
[[nodiscard]] bool is_cjk_cp(char32_t cp) noexcept;
[[nodiscard]] bool is_latin_letter(char32_t cp) noexcept;
[[nodiscard]] bool is_space_cp(char32_t cp) noexcept;
[[nodiscard]] std::vector<char32_t> to_codepoints(std::string_view s);

/// Collapses all Unicode whitespace (ASCII, NBSP, U+3000, etc.) into single ASCII ' ' and strips edges.
[[nodiscard]] std::string normalize_whitespace(std::string_view text);

/// Alias for normalize_whitespace (SubLift parity)
[[nodiscard]] inline std::string normalize_ocr_text(std::string_view text) {
  return normalize_whitespace(text);
}

/// Normalizes CJK punctuation, strips edge attached Latin noise, and converts trailing fact dot.
[[nodiscard]] std::string cleanup_subtitle_text(
    std::string_view text, std::string_view script = SCRIPT_CJK);

/// Computes the ratio of CJK ideographs to non-space characters.
[[nodiscard]] double cjk_ratio(std::string_view text);

/// Computes the ratio of Latin letters to non-space characters.
[[nodiscard]] double latin_ratio(std::string_view text);

/// Computes script match score [0.0, 1.0] given expected script ("cjk", "latin", "auto").
[[nodiscard]] double script_score(std::string_view text, std::string_view script);

} // namespace sublift::text
