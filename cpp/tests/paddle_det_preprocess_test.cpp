#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include "ppocr_det_preprocess.hpp"

using Catch::Approx;

TEST_CASE(
    "Paddle Det preprocess matches RapidOCR min-736 shape and BGR NCHW",
    "[paddle][det][preprocess]") {
  constexpr std::int32_t kWidth = 960;
  constexpr std::int32_t kHeight = 256;
  std::vector<std::uint8_t> rgb(
      static_cast<std::size_t>(kWidth) * kHeight * 3);
  for (std::size_t index = 0; index < rgb.size(); index += 3) {
    rgb[index] = 255;
    rgb[index + 1] = 0;
    rgb[index + 2] = 128;
  }

  const auto global = sublift::paddle::prepare_global_image(
      rgb.data(), kWidth, kHeight, kWidth * 3);
  REQUIRE(global.width == kWidth);
  REQUIRE(global.height == kHeight);
  REQUIRE(global.padding_top == 0);
  REQUIRE(global.ratio_h == Approx(1.0));
  REQUIRE(global.ratio_w == Approx(1.0));

  const auto det = sublift::paddle::prepare_det_input(
      global.rgb.data(), global.width, global.height);
  REQUIRE(det.width == 2752);
  REQUIRE(det.height == 736);
  REQUIRE(det.nchw.size() == 3U * 2752U * 736U);
  const std::size_t plane = 2752U * 736U;
  CHECK(det.nchw[0] == Approx(128.0F / 255.0F * 2.0F - 1.0F));
  CHECK(det.nchw[plane] == Approx(-1.0F));
  CHECK(det.nchw[plane * 2] == Approx(1.0F));
}

TEST_CASE(
    "Paddle global preprocess reproduces RapidOCR vertical padding",
    "[paddle][det][preprocess]") {
  constexpr std::int32_t kWidth = 100;
  constexpr std::int32_t kHeight = 10;
  constexpr std::int32_t kStride = kWidth * 3 + 7;
  std::vector<std::uint8_t> strided(
      static_cast<std::size_t>(kStride) * kHeight, 255);

  const auto global = sublift::paddle::prepare_global_image(
      strided.data(), kWidth, kHeight, kStride);
  CHECK(global.width == 288);
  CHECK(global.height == 72);
  CHECK(global.padding_top == 20);
  CHECK(global.padding_left == 0);
  CHECK(global.ratio_h == Approx(10.0 / 32.0));
  CHECK(global.ratio_w == Approx(100.0 / 288.0));
  REQUIRE(global.rgb.size() == 288U * 72U * 3U);
  CHECK(global.rgb.front() == 0);
  CHECK(global.rgb[20U * 288U * 3U] == 255);
}
