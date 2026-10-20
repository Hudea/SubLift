#include <catch2/catch_test_macros.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>

#include "sublift/image.hpp"
#include "sublift/signature.hpp"

namespace {

sublift::ImageBuffer make_pattern(std::int32_t w, std::int32_t h, int shift) {
  sublift::ImageBuffer img(w, h, sublift::PixelFormat::Gray8);
  for (std::int32_t y = 0; y < h; ++y) {
    for (std::int32_t x = 0; x < w; ++x) {
      const int base = (x * 7 + y * 13 + shift * 29) % 251;
      img.data()[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                  static_cast<std::size_t>(x)] = static_cast<std::uint8_t>(base);
    }
  }
  return img;
}

// Exact (bit-level) equality: O1/O2 claim bit-identical numerics, so plain
// double comparison would not state the invariant strongly enough.
std::uint64_t bits(double value) { return std::bit_cast<std::uint64_t>(value); }

}  // namespace

TEST_CASE("foreground mask is deterministic for identical input", "[signature][patrol]") {
  const auto image = make_pattern(48, 32, 3);
  const auto first = sublift::compute_foreground_mask(image.view());
  const auto second = sublift::compute_foreground_mask(image.view());
  REQUIRE(first.width() == second.width());
  REQUIRE(first.height() == second.height());
  REQUIRE(first.stride_bytes() == second.stride_bytes());
  const std::size_t nbytes =
      static_cast<std::size_t>(first.stride_bytes()) * static_cast<std::size_t>(first.height());
  for (std::size_t i = 0; i < nbytes; ++i) {
    REQUIRE(first.data()[i] == second.data()[i]);
  }
}

TEST_CASE("mask SSIM with a reused workspace stays bit-identical (O2)", "[signature][patrol]") {
  const auto a = make_pattern(48, 32, 1);
  const auto b = make_pattern(48, 32, 2);
  const auto c = make_pattern(48, 32, 9);
  const auto m_a = sublift::compute_foreground_mask(a.view());
  const auto m_b = sublift::compute_foreground_mask(b.view());
  const auto m_c = sublift::compute_foreground_mask(c.view());

  sublift::SsimWorkspace workspace;
  const std::uint64_t first = bits(sublift::compute_mask_ssim_reusing(
      m_a.view(), m_b.view(), 7, workspace));
  const std::uint64_t reference =
      bits(sublift::compute_mask_ssim(m_a.view(), m_b.view(), 7));
  REQUIRE(first == reference);

  // Reusing the workspace for other pairs, and repeating a pair, must not leak
  // state: every call still matches the allocating reference exactly.
  REQUIRE(bits(sublift::compute_mask_ssim_reusing(m_a.view(), m_c.view(), 7, workspace)) ==
          bits(sublift::compute_mask_ssim(m_a.view(), m_c.view(), 7)));
  REQUIRE(bits(sublift::compute_mask_ssim_reusing(m_b.view(), m_c.view(), 7, workspace)) ==
          bits(sublift::compute_mask_ssim(m_b.view(), m_c.view(), 7)));
  REQUIRE(bits(sublift::compute_mask_ssim_reusing(m_a.view(), m_b.view(), 7, workspace)) ==
          first);

  // A fresh workspace yields the same numbers as the reused one.
  sublift::SsimWorkspace fresh;
  REQUIRE(bits(sublift::compute_mask_ssim_reusing(m_a.view(), m_b.view(), 7, fresh)) == first);
}

TEST_CASE("cached anchor-mask decomposition equals the combined masked SSIM (O1)",
          "[signature][patrol]") {
  const auto current = make_pattern(48, 32, 4);
  const auto anchor = make_pattern(48, 32, 5);
  const sublift::SignatureConfig config{};

  // Pre-optimization path: compute both masks inside compute_foreground_ssim.
  const double combined = sublift::compute_foreground_ssim(
      current.view(), anchor.view(), config, /*use_mask=*/true, 7);

  // O1 + O2 path: anchor mask cached once, current mask per patrol, SSIM over
  // the two masks with the reusable workspace.
  const auto current_mask = sublift::compute_foreground_mask(current.view(), config);
  const auto anchor_mask = sublift::compute_foreground_mask(anchor.view(), config);
  sublift::SsimWorkspace workspace;
  const double cached = sublift::compute_mask_ssim_reusing(
      current_mask.view(), anchor_mask.view(), 7, workspace);

  REQUIRE(bits(cached) == bits(combined));
}
