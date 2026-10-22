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
    "Paddle Det geometry keeps the RapidOCR min-736 default",
    "[paddle][det][preprocess]") {
  using sublift::paddle::DetInputGeometry;
  using sublift::paddle::det_input_size;

  CHECK(det_input_size(1920, 324, DetInputGeometry{}) ==
        sublift::paddle::DetInputSize{4352, 736});
  CHECK(det_input_size(960, 256, DetInputGeometry{}) ==
        sublift::paddle::DetInputSize{2752, 736});
}

TEST_CASE(
    "Paddle Det geometry supports a pixel budget without downscaling",
    "[paddle][det][preprocess]") {
  using sublift::paddle::DetInputGeometry;
  using sublift::paddle::det_input_size;

  const DetInputGeometry budget{736.0, 700000.0};
  CHECK(det_input_size(1920, 324, budget) ==
        sublift::paddle::DetInputSize{2048, 352});
  CHECK(det_input_size(960, 256, budget) ==
        sublift::paddle::DetInputSize{1632, 448});

  const DetInputGeometry smaller_than_native{736.0, 400000.0};
  CHECK(det_input_size(1920, 324, smaller_than_native) ==
        sublift::paddle::DetInputSize{1920, 320});

  const DetInputGeometry native{0.0, 0.0};
  CHECK(det_input_size(1920, 324, native) ==
        sublift::paddle::DetInputSize{1920, 320});
}

TEST_CASE(
    "Paddle Det geometry applies the conditional short-side floor",
    "[paddle][det][preprocess]") {
  using sublift::paddle::DetInputGeometry;
  using sublift::paddle::det_input_size;

  // 720p 30% band: 1280x216. The area policy already lands at a 320px short
  // side; the 352 floor must raise it further without downscaling.
  const auto no_floor = det_input_size(1280, 216, DetInputGeometry{736.0, 600000.0});
  const auto floor320 =
      det_input_size(1280, 216, DetInputGeometry{736.0, 600000.0, 320.0});
  const auto floor352 =
      det_input_size(1280, 216, DetInputGeometry{736.0, 600000.0, 352.0});
  CHECK(no_floor.height >= 320);
  CHECK(floor320.height >= 320);
  CHECK(floor352.height >= 352);
  CHECK(floor352.height > no_floor.height);
  CHECK(floor352.width % 32 == 0);
  CHECK(floor352.height % 32 == 0);
  CHECK(static_cast<double>(floor352.width) * floor352.height >= 600000.0);

  // 22.5% band: 1920x243. The area-only short side is below 320; the floor
  // must lift it.
  const auto p225_no = det_input_size(1920, 243, DetInputGeometry{736.0, 600000.0});
  const auto p225_320 =
      det_input_size(1920, 243, DetInputGeometry{736.0, 600000.0, 320.0});
  const auto p225_352 =
      det_input_size(1920, 243, DetInputGeometry{736.0, 600000.0, 352.0});
  CHECK(p225_no.height < 320);
  CHECK(p225_320.height >= 320);
  CHECK(p225_352.height >= 352);
  CHECK(p225_352.width > p225_320.width);
}

TEST_CASE(
    "Paddle Det preprocess applies the pixel budget geometry",
    "[paddle][det][preprocess]") {
  constexpr std::int32_t kWidth = 960;
  constexpr std::int32_t kHeight = 256;
  std::vector<std::uint8_t> rgb(
      static_cast<std::size_t>(kWidth) * kHeight * 3, 128);

  const auto det = sublift::paddle::prepare_det_input(
      rgb.data(), kWidth, kHeight,
      sublift::paddle::DetInputGeometry{736.0, 700000.0});
  CHECK(det.width == 1632);
  CHECK(det.height == 448);
  CHECK(det.nchw.size() == 3U * 1632U * 448U);
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
