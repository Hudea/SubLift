#include "sublift/vision.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace sublift {

namespace {

/// Match Python `str.isspace()` for ASCII + common Unicode Zs (same set as dedupe).
[[nodiscard]] bool is_python_space_utf8(std::string_view text, std::size_t& i) noexcept {
  if (i >= text.size()) {
    return false;
  }
  const auto c = static_cast<unsigned char>(text[i]);
  if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') {
    ++i;
    return true;
  }
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

}  // namespace

bool is_ocr_text_blank(std::string_view text) noexcept {
  // Python: not text.strip()
  std::size_t i = 0;
  while (i < text.size()) {
    const std::size_t before = i;
    if (is_python_space_utf8(text, i)) {
      continue;
    }
    if (i == before) {
      return false;  // non-space content
    }
  }
  return true;
}

OcrCropBox clamp_ocr_box(
    std::int32_t x, std::int32_t y,
    std::int32_t w, std::int32_t h,
    std::int32_t image_width, std::int32_t image_height) noexcept {
  if (image_width <= 0 || image_height <= 0) {
    return OcrCropBox{0, 0, 0, 0};
  }

  const auto clamped_x = std::max<std::int32_t>(0, std::min<std::int32_t>(x, image_width));
  const auto clamped_y = std::max<std::int32_t>(0, std::min<std::int32_t>(y, image_height));
  const auto max_w = image_width - clamped_x;
  const auto max_h = image_height - clamped_y;
  const auto clamped_w = std::max<std::int32_t>(0, std::min<std::int32_t>(w, max_w));
  const auto clamped_h = std::max<std::int32_t>(0, std::min<std::int32_t>(h, max_h));

  return OcrCropBox{clamped_x, clamped_y, clamped_w, clamped_h};
}

OcrCropBox vision_normalized_box_to_pixel(
    double nx, double ny, double nw, double nh,
    std::int32_t image_width, std::int32_t image_height) noexcept {
  const auto x = bankers_round(nx * static_cast<double>(image_width));
  const auto y = bankers_round((1.0 - ny - nh) * static_cast<double>(image_height));
  const auto w = bankers_round(nw * static_cast<double>(image_width));
  const auto h = bankers_round(nh * static_cast<double>(image_height));

  return clamp_ocr_box(x, y, w, h, image_width, image_height);
}

void sort_ocr_lines(std::span<OcrLine> lines) {
  std::stable_sort(lines.begin(), lines.end(), [](const OcrLine& a, const OcrLine& b) noexcept {
    if (a.box.y != b.box.y) {
      return a.box.y < b.box.y;
    }
    return a.box.x < b.box.x;
  });
}

}  // namespace sublift
