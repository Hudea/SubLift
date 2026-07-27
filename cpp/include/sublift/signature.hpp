#pragma once

#include <cstdint>

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

}  // namespace sublift
