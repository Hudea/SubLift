#pragma once

#include <cstdint>
#include <vector>

namespace sublift::paddle {

/// Packed RGB image after RapidOCR's global resize and optional vertical pad.
/// ratio_h/ratio_w map the pre-padding working coordinates back to the source.
struct GlobalPreprocessResult {
  std::vector<std::uint8_t> rgb;
  std::int32_t width{0};
  std::int32_t height{0};
  double ratio_h{1.0};
  double ratio_w{1.0};
  std::int32_t padding_top{0};
  std::int32_t padding_left{0};
};

struct DetPreprocessResult {
  std::vector<float> nchw;
  std::int32_t width{0};
  std::int32_t height{0};
};

/// Mirror RapidOCR 3.9.2 Global preprocessing:
/// resize_image_within_bounds(30, 2000), then vertical padding when h <= 30
/// or w / h > 8. Input/output remain RGB because interpolation is
/// channel-independent; Det conversion to BGR happens in prepare_det_input().
[[nodiscard]] GlobalPreprocessResult prepare_global_image(
    const std::uint8_t* rgb,
    std::int32_t width,
    std::int32_t height,
    std::int32_t stride);

/// Mirror RapidOCR DetPreProcess(limit_side_len=736, limit_type="min"):
/// INTER_LINEAR resize to multiples of 32, RGB -> BGR, float normalization,
/// HWC -> NCHW.
[[nodiscard]] DetPreprocessResult prepare_det_input(
    const std::uint8_t* packed_rgb,
    std::int32_t width,
    std::int32_t height);

}  // namespace sublift::paddle
