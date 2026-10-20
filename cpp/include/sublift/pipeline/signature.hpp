#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>

#include "sublift/config.hpp"
#include "sublift/image.hpp"

namespace sublift {

/// Frame signature (matches src/sublift/pipeline/signature.py FrameSignature).
///
/// `dhash` is a 64-bit difference hash; `hash_size` must be <= 8 so the hash
/// fits in `uint64_t` (Python `int` is unbounded; 6.1 locks `hash_size == 8`).
struct FrameSignature {
  std::int64_t timestamp_ms{0};
  double foreground_ratio{0.0};
  std::uint64_t dhash{0};

  friend constexpr bool operator==(const FrameSignature&,
                                   const FrameSignature&) = default;
};

/// Compute the full frame signature (foreground ratio + dHash).
/// Matches `compute_signature`. Reproduces the historical color-space quirk:
/// 3-channel views are fed through `COLOR_RGB2GRAY` regardless of byte order,
/// so a buffer whose bytes are actually BGR but tagged `RGB24` is processed
/// exactly as Python `_to_gray` does. Do NOT "fix" this to `BGR2GRAY` in 6.1.
[[nodiscard]] FrameSignature compute_signature(
    const ImageView& image, std::int64_t timestamp_ms,
    const SignatureConfig& config = SignatureConfig{});

/// Difference hash of a grayscale/binary image. Matches `compute_dhash`.
/// `hash_size` must be in [1, 8]; input must be `Gray8`.
[[nodiscard]] std::uint64_t compute_dhash(const ImageView& gray,
                                          std::int32_t hash_size = 8);

/// Hamming distance (popcount of XOR). Matches `hamming_distance`.
[[nodiscard]] int hamming_distance(std::uint64_t a, std::uint64_t b) noexcept;

/// Structural similarity between two images. Matches `compute_ssim` (numpy
/// fallback over `GaussianBlur`). Inputs are converted to grayscale first.
/// L1 epsilon parity (see parity-contract.md §4).
[[nodiscard]] double compute_ssim(const ImageView& image1, const ImageView& image2,
                                  std::int32_t window_size = 7);

/// Foreground SSIM (feat-031b patrol). Matches `compute_foreground_ssim`:
/// when `use_mask`, SSIM is computed on the binarized foreground masks (via
/// `_compute_foreground_ratio`) rather than raw grayscale crops.
[[nodiscard]] double compute_foreground_ssim(
    const ImageView& current, const ImageView& anchor,
    const SignatureConfig& config = SignatureConfig{}, bool use_mask = true,
    std::int32_t window_size = 7);

/// Foreground binary mask (adaptiveThreshold GAUSSIAN_C, THRESH_BINARY_INV ->
/// MORPH_OPEN 2x2 rect) as an owned Gray8 buffer (0/255 per pixel).
///
/// Matches the `binary` half of `_compute_foreground_ratio` (including the
/// historical RGB2GRAY color quirk via `to_gray`). Exposed for the patrol
/// anchor-mask cache (O1): the anchor crop is immutable between In/CHANGE,
/// so its mask can be computed once and reused.
[[nodiscard]] ImageBuffer compute_foreground_mask(
    const ImageView& image, const SignatureConfig& config = SignatureConfig{});

/// SSIM between two Gray8 foreground masks. Numerically identical to the
/// masked branch of `compute_foreground_ssim` (same CV_64F pipeline over the
/// two binary masks).
[[nodiscard]] double compute_mask_ssim(const ImageView& mask1,
                                       const ImageView& mask2,
                                       std::int32_t window_size = 7);

/// Reusable SSIM workspace (O2). Holds the CV_64F intermediates so repeated
/// patrol SSIM calls avoid ~15 MB-scale allocations each. Opaque: no OpenCV
/// types leak into the public API (architecture.md §3).
///
/// Numerics are unchanged when used through `compute_mask_ssim_reusing`:
/// same operations, same order, same CV_64F types and blur parameters - only
/// the destination Mats are reused (OpenCV reuses a Mat's buffer when its
/// size/type already match).
class SsimWorkspace {
 public:
  SsimWorkspace();
  ~SsimWorkspace();
  SsimWorkspace(SsimWorkspace&& other) noexcept;
  SsimWorkspace& operator=(SsimWorkspace&& other) noexcept;
  SsimWorkspace(const SsimWorkspace&) = delete;
  SsimWorkspace& operator=(const SsimWorkspace&) = delete;

  /// Opaque state (defined in signature.cpp); public so the workspace-aware
  /// SSIM core can take a pointer to it without exposing cv::Mat.
  struct Impl;

 private:
  std::unique_ptr<Impl> impl_;
  friend double compute_mask_ssim_reusing(const ImageView& mask1,
                                          const ImageView& mask2,
                                          std::int32_t window_size,
                                          SsimWorkspace& workspace);
};

/// SSIM between two Gray8 masks with a caller-held reusable workspace (O2).
/// Bit-identical to `compute_mask_ssim` for the same inputs.
[[nodiscard]] double compute_mask_ssim_reusing(const ImageView& mask1,
                                               const ImageView& mask2,
                                               std::int32_t window_size,
                                               SsimWorkspace& workspace);

}  // namespace sublift
