#pragma once

#include <cstdint>
#include <vector>

#include <sublift/adapters/paddle/paddle_ocr.hpp>

namespace sublift::adapters::paddle::detail {

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
/// Invalid or non-positive values fall back to the defaults.
[[nodiscard]] DetInputGeometry det_input_geometry_from_env() noexcept;

struct DetInputSize {
  std::int32_t width{0};
  std::int32_t height{0};

  friend constexpr bool operator==(const DetInputSize&,
                                   const DetInputSize&) = default;
};

/// Resolves the resized Det tensor geometry for a source crop.
[[nodiscard]] DetInputSize det_input_size(
    std::int32_t width,
    std::int32_t height,
    const DetInputGeometry& geometry) noexcept;

/// Prepare a source RGB crop for DBNet + Rec crop extraction
[[nodiscard]] GlobalPreprocessResult prepare_global_image(
    const std::uint8_t* rgb,
    std::int32_t width,
    std::int32_t height,
    std::int32_t stride);

/// Fill a caller-provided GlobalPreprocessResult, reusing its `rgb` vector
void prepare_global_image_into(
    const std::uint8_t* rgb,
    std::int32_t width,
    std::int32_t height,
    std::int32_t stride,
    GlobalPreprocessResult* out);

/// Mirror RapidOCR DetPreProcess
[[nodiscard]] DetPreprocessResult prepare_det_input(
    const std::uint8_t* packed_rgb,
    std::int32_t width,
    std::int32_t height);

[[nodiscard]] DetPreprocessResult prepare_det_input(
    const std::uint8_t* packed_rgb,
    std::int32_t width,
    std::int32_t height,
    const DetInputGeometry& geometry);

/// Fill a caller-provided contiguous NCHW float tensor instead of allocating a fresh vector.
[[nodiscard]] DetInputSize prepare_det_input_into(
    const std::uint8_t* packed_rgb,
    std::int32_t width,
    std::int32_t height,
    const DetInputGeometry& geometry,
    float* out_nchw);

}  // namespace sublift::adapters::paddle::detail
