#pragma once

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

#include "sublift/models.hpp"

namespace sublift {

/// 2D 浮点坐标点
struct Point2D {
  float x{0.0f};
  float y{0.0f};
};

/// 4 个 (x, y) 浮点坐标构成的四角点
using QuadCorners = std::array<Point2D, 4>;

/// 1. 将四角点 (float/double) 映射为轴对齐包围盒 (AABB)
/// 算法对齐 Python paddle.py:
///   x_min = int(floor(xs.min())), y_min = int(floor(ys.min()))
///   x_max = int(ceil(xs.max())),  y_max = int(ceil(ys.max()))
OcrCropBox quad_to_aabb(const QuadCorners& corners);

/// 2. 将包围盒限制在 [0, img_w] x [0, img_h]
/// 算法对齐 Python paddle.py:
///   x_min = max(0, min(x_min, img_w)), y_min = max(0, min(y_min, img_h))
///   x_max = max(0, min(x_max, img_w)), y_max = max(0, min(y_max, img_h))
OcrCropBox clamp_box(const OcrCropBox& box, int32_t img_w, int32_t img_h);

/// 3. 组合转换：quad -> aabb -> clamp
OcrCropBox quad_to_clamped_aabb(const QuadCorners& corners, int32_t img_w, int32_t img_h);

/// 4. 稳定行序：优先上->下 (y)，同 y 则左->右 (x)
void sort_ocr_lines(std::vector<OcrLine>& lines);

/// 5. 判断字符串经过 strip (去除首尾空白字符: ' ', '\t', '\r', '\n') 后是否为空
bool is_strip_empty(std::string_view text);

/// 6. RGB24 内存 -> BGR24 内存转换（防止按 BGR 直接消费 RGB 导致红蓝颠倒）
/// @param src 连续或带 stride 的 RGB 内存缓冲区
/// @param dst 接收 BGR 的内存缓冲区
/// @param width 宽度（像素数）
/// @param height 高度（像素数）
/// @param src_stride 输入跨距（字节），<= 0 表示紧凑存储 (width * 3)
/// @param dst_stride 输出跨距（字节），<= 0 表示紧凑存储 (width * 3)
void rgb24_to_bgr24(const uint8_t* src,
                    uint8_t* dst,
                    int32_t width,
                    int32_t height,
                    int32_t src_stride = 0,
                    int32_t dst_stride = 0);

}  // namespace sublift
