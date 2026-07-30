#include "sublift/paddle_geometry.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace sublift {

OcrCropBox quad_to_aabb(const QuadCorners& corners) {
  float min_x = corners[0].x;
  float max_x = corners[0].x;
  float min_y = corners[0].y;
  float max_y = corners[0].y;

  for (size_t i = 1; i < corners.size(); ++i) {
    min_x = std::min(min_x, corners[i].x);
    max_x = std::max(max_x, corners[i].x);
    min_y = std::min(min_y, corners[i].y);
    max_y = std::max(max_y, corners[i].y);
  }

  int32_t x_min = static_cast<int32_t>(std::floor(min_x));
  int32_t y_min = static_cast<int32_t>(std::floor(min_y));
  int32_t x_max = static_cast<int32_t>(std::ceil(max_x));
  int32_t y_max = static_cast<int32_t>(std::ceil(max_y));

  int32_t w = std::max(0, x_max - x_min);
  int32_t h = std::max(0, y_max - y_min);

  return OcrCropBox{x_min, y_min, w, h};
}

OcrCropBox clamp_box(const OcrCropBox& box, int32_t img_w, int32_t img_h) {
  if (img_w <= 0 || img_h <= 0) {
    return OcrCropBox{0, 0, 0, 0};
  }

  int32_t x_min = box.x;
  int32_t y_min = box.y;
  int32_t x_max = box.x + box.width;
  int32_t y_max = box.y + box.height;

  x_min = std::max(0, std::min(x_min, img_w));
  y_min = std::max(0, std::min(y_min, img_h));
  x_max = std::max(0, std::min(x_max, img_w));
  y_max = std::max(0, std::min(y_max, img_h));

  return OcrCropBox{x_min, y_min, x_max - x_min, y_max - y_min};
}

OcrCropBox quad_to_clamped_aabb(const QuadCorners& corners, int32_t img_w, int32_t img_h) {
  return clamp_box(quad_to_aabb(corners), img_w, img_h);
}

void sort_ocr_lines(std::vector<OcrLine>& lines) {
  std::stable_sort(lines.begin(), lines.end(), [](const OcrLine& a, const OcrLine& b) {
    if (a.box.y != b.box.y) {
      return a.box.y < b.box.y;
    }
    return a.box.x < b.box.x;
  });
}

bool is_strip_empty(std::string_view text) {
  auto start = text.find_first_not_of(" \t\r\n");
  return start == std::string_view::npos;
}

void rgb24_to_bgr24(const uint8_t* src,
                    uint8_t* dst,
                    int32_t width,
                    int32_t height,
                    int32_t src_stride,
                    int32_t dst_stride) {
  if (!src || !dst || width <= 0 || height <= 0) {
    return;
  }

  int32_t s_stride = src_stride > 0 ? src_stride : width * 3;
  int32_t d_stride = dst_stride > 0 ? dst_stride : width * 3;

  for (int32_t y = 0; y < height; ++y) {
    const uint8_t* s_row = src + static_cast<size_t>(y) * s_stride;
    uint8_t* d_row = dst + static_cast<size_t>(y) * d_stride;
    for (int32_t x = 0; x < width; ++x) {
      uint8_t r = s_row[x * 3 + 0];
      uint8_t g = s_row[x * 3 + 1];
      uint8_t b = s_row[x * 3 + 2];

      d_row[x * 3 + 0] = b;
      d_row[x * 3 + 1] = g;
      d_row[x * 3 + 2] = r;
    }
  }
}

}  // namespace sublift
