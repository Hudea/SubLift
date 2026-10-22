#pragma once

#include <cstdint>
#include <vector>

#include "sublift/det_geometry.hpp"

namespace sublift::paddle {

using sublift::DetInputGeometry;

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

/// Reads the diagnostic experiment overrides:
///   SUBLIFT_DET_MIN_SIDE=<side>      min-side target (default 736)
///   SUBLIFT_DET_PIXEL_BUDGET=<area>  target area; takes precedence over min side
/// Invalid or non-positive values fall back to the defaults. Product config
/// flows through `PaddleOptions::det_geometry` instead; this function remains
/// for diagnostics and direct adapter tests.
[[nodiscard]] DetInputGeometry det_input_geometry_from_env() noexcept;

struct DetInputSize {
  std::int32_t width{0};
  std::int32_t height{0};

  friend constexpr bool operator==(const DetInputSize&,
                                   const DetInputSize&) = default;
};

/// Resolves the resized Det tensor geometry for a source crop. The result is
/// always a multiple of 32; the policy never downscales, but 32-multiple
/// rounding can reduce each side by up to 16 px.
[[nodiscard]] DetInputSize det_input_size(
    std::int32_t width,
    std::int32_t height,
    const DetInputGeometry& geometry) noexcept;

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
/// HWC -> NCHW. The three-argument form reads the diagnostic env overrides;
/// the four-argument form uses the supplied geometry.
[[nodiscard]] DetPreprocessResult prepare_det_input(
    const std::uint8_t* packed_rgb,
    std::int32_t width,
    std::int32_t height);

[[nodiscard]] DetPreprocessResult prepare_det_input(
    const std::uint8_t* packed_rgb,
    std::int32_t width,
    std::int32_t height,
    const DetInputGeometry& geometry);

/// Fill a caller-provided contiguous NCHW float tensor instead of allocating
/// a fresh vector. The caller must provide enough storage for
/// det_input_size(width, height, geometry) * 3 floats; this form exists for
/// hot loops that reuse one buffer across frames (Phase 20). Returns the
/// resolved input geometry.
[[nodiscard]] DetInputSize prepare_det_input_into(
    const std::uint8_t* packed_rgb,
    std::int32_t width,
    std::int32_t height,
    const DetInputGeometry& geometry,
    float* out_nchw);

}  // namespace sublift::paddle
