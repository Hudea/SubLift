#include <catch2/catch_test_macros.hpp>

#include "sublift/image.hpp"
#include "sublift/signature.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace {

sublift::ImageBuffer make_gray(std::int32_t w, std::int32_t h, std::uint8_t val) {
  sublift::ImageBuffer img(w, h, sublift::PixelFormat::Gray8);
  std::fill(img.data(), img.data() + static_cast<std::size_t>(w) * h, val);
  return img;
}

sublift::ImageBuffer make_gray_pixels(std::int32_t w, std::int32_t h,
                                      const std::vector<std::uint8_t>& vals) {
  sublift::ImageBuffer img(w, h, sublift::PixelFormat::Gray8);
  std::copy(vals.begin(), vals.end(), img.data());
  return img;
}

}  // namespace

TEST_CASE("dhash of a uniform gray image is zero", "[signature]") {
  auto img = make_gray(16, 8, 100);
  REQUIRE(sublift::compute_dhash(img.view()) == 0ULL);
}

TEST_CASE("dhash strict-greater bit packing (hash_size=1)", "[signature]") {
  // 1x2 (h=1,w=2): resize to Size(2,1) via INTER_AREA is identity, so the bit
  // is exactly (resized[0][1] > resized[0][0]). Strict `>`: equal -> 0.
  REQUIRE(sublift::compute_dhash(make_gray_pixels(2, 1, {100, 200}).view(), 1) == 1ULL);
  REQUIRE(sublift::compute_dhash(make_gray_pixels(2, 1, {200, 100}).view(), 1) == 0ULL);
  REQUIRE(sublift::compute_dhash(make_gray_pixels(2, 1, {100, 100}).view(), 1) == 0ULL);
}

TEST_CASE("hamming distance", "[signature]") {
  REQUIRE(sublift::hamming_distance(0, 0) == 0);
  REQUIRE(sublift::hamming_distance(0xFFu, 0x00) == 8);
  REQUIRE(sublift::hamming_distance(0xAAu, 0x55) == 8);
  REQUIRE(sublift::hamming_distance(0x0Fu, 0xF0) == 8);
  REQUIRE(sublift::hamming_distance(0b1010u, 0b0101) == 4);
}

TEST_CASE("compute_ssim identical vs differing uniform images", "[signature]") {
  auto a = make_gray(8, 8, 100);
  auto b = make_gray(8, 8, 100);
  auto c = make_gray(8, 8, 200);
  REQUIRE(std::abs(sublift::compute_ssim(a.view(), b.view()) - 1.0) < 1e-6);
  REQUIRE(sublift::compute_ssim(a.view(), c.view()) < 1.0);
}

TEST_CASE("compute_dhash rejects non-Gray8 and bad hash_size", "[signature]") {
  sublift::ImageBuffer rgb(4, 4, sublift::PixelFormat::RGB24);
  REQUIRE_THROWS_AS(sublift::compute_dhash(rgb.view(), 8), std::invalid_argument);
  auto g = make_gray(4, 4, 0);
  REQUIRE_THROWS_AS(sublift::compute_dhash(g.view(), 0), std::invalid_argument);
  REQUIRE_THROWS_AS(sublift::compute_dhash(g.view(), 9), std::invalid_argument);
}

TEST_CASE("compute_signature on uniform gray yields zero fg_ratio and dhash",
          "[signature]") {
  auto img = make_gray(32, 16, 50);
  const auto sig = sublift::compute_signature(img.view(), 12345);
  REQUIRE(sig.timestamp_ms == 12345);
  REQUIRE(sig.dhash == 0ULL);
  REQUIRE(sig.foreground_ratio == 0.0);
}

TEST_CASE("compute_foreground_ssim mask vs raw paths", "[signature]") {
  // Fixture-scale (128x64) mid-gray bg + offset bright bands → non-empty
  // adaptive masks. Tiny pure 0/255 crops often yield empty BINARY_INV masks.
  constexpr std::int32_t w = 128;
  constexpr std::int32_t h = 64;
  auto left = make_gray(w, h, 40);
  auto right = make_gray(w, h, 40);
  for (std::int32_t y = 20; y < 44; ++y) {
    for (std::int32_t x = 16; x < 48; ++x) {
      left.data()[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                  static_cast<std::size_t>(x)] = 240;
    }
    for (std::int32_t x = 64; x < 96; ++x) {
      right.data()[static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                   static_cast<std::size_t>(x)] = 240;
    }
  }
  const double same_mask = sublift::compute_foreground_ssim(
      left.view(), left.view(), {}, /*use_mask=*/true);
  const double same_raw = sublift::compute_foreground_ssim(
      left.view(), left.view(), {}, /*use_mask=*/false);
  REQUIRE(std::abs(same_mask - 1.0) < 1e-6);
  REQUIRE(std::abs(same_raw - 1.0) < 1e-6);

  const double diff_mask = sublift::compute_foreground_ssim(
      left.view(), right.view(), {}, /*use_mask=*/true);
  const double diff_raw = sublift::compute_foreground_ssim(
      left.view(), right.view(), {}, /*use_mask=*/false);
  REQUIRE(diff_mask < same_mask);
  REQUIRE(diff_raw < same_raw);
  REQUIRE(diff_mask >= 0.0);
  REQUIRE(diff_raw >= 0.0);
}

TEST_CASE("RGB2GRAY quirk diverges from channel-swapped path", "[signature]") {
  // Yellow (255,255,0): quirk RGB2GRAY ≈ 226 (non-zero mask); channel-swap then
  // RGB2GRAY ≈ BGR2GRAY ≈ 179 which often yields empty mask on dark bg.
  constexpr std::int32_t w = 128;
  constexpr std::int32_t h = 64;
  sublift::ImageBuffer quirk(w, h, sublift::PixelFormat::RGB24);
  std::fill(quirk.data(), quirk.data() + static_cast<std::size_t>(w) * h * 3,
            static_cast<std::uint8_t>(40));
  for (std::int32_t y = 44; y < 60; ++y) {
    for (std::int32_t x = 60; x < 92; ++x) {
      auto* p = quirk.data() +
                (static_cast<std::size_t>(y) * static_cast<std::size_t>(w) +
                 static_cast<std::size_t>(x)) *
                    3;
      p[0] = 255;
      p[1] = 255;
      p[2] = 0;
    }
  }
  sublift::ImageBuffer swapped(w, h, sublift::PixelFormat::RGB24);
  for (std::size_t i = 0, n = static_cast<std::size_t>(w) * h; i < n; ++i) {
    swapped.data()[i * 3 + 0] = quirk.data()[i * 3 + 2];
    swapped.data()[i * 3 + 1] = quirk.data()[i * 3 + 1];
    swapped.data()[i * 3 + 2] = quirk.data()[i * 3 + 0];
  }
  const auto sq = sublift::compute_signature(quirk.view(), 1);
  const auto sf = sublift::compute_signature(swapped.view(), 1);
  REQUIRE((sq.dhash != sf.dhash ||
           sq.foreground_ratio != sf.foreground_ratio));
}
