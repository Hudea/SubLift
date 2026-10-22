#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <array>
#include <cstdint>
#include <cstring>
#include <random>
#include <vector>

#include <opencv2/imgproc.hpp>

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

namespace {

std::vector<float> reference_det_nchw(const std::vector<std::uint8_t>& rgb,
                                      std::int32_t width,
                                      std::int32_t height,
                                      const sublift::paddle::DetInputGeometry& geometry,
                                      int* out_width,
                                      int* out_height) {
  const auto size = sublift::paddle::det_input_size(width, height, geometry);
  *out_width = size.width;
  *out_height = size.height;
  cv::Mat source(height, width, CV_8UC3,
                 const_cast<std::uint8_t*>(rgb.data()));
  cv::Mat resized;
  cv::resize(source, resized, cv::Size(size.width, size.height), 0.0, 0.0,
             cv::INTER_LINEAR);
  const auto normalize = [](std::uint8_t value) {
    volatile float scaled = static_cast<float>(value) * (1.0F / 255.0F);
    volatile float centered = scaled - 0.5F;
    return centered / 0.5F;
  };
  const auto plane = static_cast<std::size_t>(size.width) *
                     static_cast<std::size_t>(size.height);
  std::vector<float> output(plane * 3U);
  for (int y = 0; y < size.height; ++y) {
    const auto* row = resized.ptr<std::uint8_t>(y);
    for (int x = 0; x < size.width; ++x) {
      const auto* pixel = row + static_cast<std::size_t>(x) * 3U;
      const auto index = static_cast<std::size_t>(y) *
                             static_cast<std::size_t>(size.width) +
                         static_cast<std::size_t>(x);
      output[index] = normalize(pixel[2]);
      output[plane + index] = normalize(pixel[1]);
      output[plane * 2U + index] = normalize(pixel[0]);
    }
  }
  return output;
}

}  // namespace

TEST_CASE("Paddle global preprocess copies strided rows without shifting",
          "[paddle][det][preprocess][stride]") {
  constexpr std::int32_t kWidth = 100;
  constexpr std::int32_t kHeight = 40;
  constexpr std::int32_t kStride = kWidth * 3 + 7;
  std::vector<std::uint8_t> strided(
      static_cast<std::size_t>(kStride) * kHeight, 0U);
  for (int row = 0; row < kHeight; ++row) {
    for (int column = 0; column < kWidth * 3; ++column) {
      strided[static_cast<std::size_t>(row) * kStride + column] =
          static_cast<std::uint8_t>((row * 31 + column * 7) & 0xFF);
    }
  }
  // 100x40 keeps max side < 2000, min side > 30 and w/h < 8, so the global
  // preprocess path performs no resize and no padding.
  const auto global = sublift::paddle::prepare_global_image(
      strided.data(), kWidth, kHeight, kStride);
  REQUIRE(global.width == kWidth);
  REQUIRE(global.height == kHeight);
  REQUIRE(global.padding_top == 0);
  REQUIRE(global.rgb.size() ==
          static_cast<std::size_t>(kWidth) * kHeight * 3U);
  bool rows_match = true;
  for (int row = 0; row < kHeight && rows_match; ++row) {
    for (int column = 0; column < kWidth * 3; ++column) {
      const auto expected = strided[static_cast<std::size_t>(row) * kStride +
                                    static_cast<std::size_t>(column)];
      const auto actual = global.rgb[static_cast<std::size_t>(row) *
                                         kWidth * 3U +
                                     static_cast<std::size_t>(column)];
      if (expected != actual) {
        rows_match = false;
        break;
      }
    }
  }
  CHECK(rows_match);
}

TEST_CASE("Paddle Det parallel preprocess is bit-identical to scalar reference",
          "[paddle][det][preprocess][equivalence]") {
  std::mt19937 rng(20261022U);
  std::uniform_int_distribution<int> pixel(0, 255);
  struct Case {
    std::int32_t width;
    std::int32_t height;
    sublift::paddle::DetInputGeometry geometry;
  };
  const std::array<Case, 5> cases{{
      {320, 96, sublift::paddle::DetInputGeometry{736.0, 600000.0}},
      {1920, 324, sublift::paddle::DetInputGeometry{736.0, 600000.0}},
      {641, 217, sublift::paddle::DetInputGeometry{736.0, 600000.0}},
      {960, 256, sublift::paddle::DetInputGeometry{736.0, 700000.0}},
      {1280, 216, sublift::paddle::DetInputGeometry{736.0, 600000.0, 352.0}},
  }};
  for (const auto& test_case : cases) {
    std::vector<std::uint8_t> rgb(
        static_cast<std::size_t>(test_case.width) *
        static_cast<std::size_t>(test_case.height) * 3U);
    for (auto& value : rgb) {
      value = static_cast<std::uint8_t>(pixel(rng));
    }
    const auto actual = sublift::paddle::prepare_det_input(
        rgb.data(), test_case.width, test_case.height, test_case.geometry);
    int expected_width = 0;
    int expected_height = 0;
    const auto expected = reference_det_nchw(
        rgb, test_case.width, test_case.height, test_case.geometry,
        &expected_width, &expected_height);
    INFO("size=" << test_case.width << "x" << test_case.height);
    REQUIRE(actual.width == expected_width);
    REQUIRE(actual.height == expected_height);
    REQUIRE(actual.nchw.size() == expected.size());
    CHECK(std::memcmp(actual.nchw.data(), expected.data(),
                      expected.size() * sizeof(float)) == 0);
  }
}
