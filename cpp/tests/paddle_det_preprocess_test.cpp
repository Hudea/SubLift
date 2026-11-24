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

  const auto global = sublift::adapters::paddle::detail::prepare_global_image(
      rgb.data(), kWidth, kHeight, kWidth * 3);
  REQUIRE(global.width == kWidth);
  REQUIRE(global.height == kHeight);
  REQUIRE(global.padding_top == 0);
  REQUIRE(global.ratio_h == Approx(1.0));
  REQUIRE(global.ratio_w == Approx(1.0));

  const auto det = sublift::adapters::paddle::detail::prepare_det_input(
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
  using sublift::adapters::paddle::DetInputGeometry;
  using sublift::adapters::paddle::detail::det_input_size;

  CHECK(det_input_size(1920, 324, DetInputGeometry{}) ==
        sublift::adapters::paddle::detail::DetInputSize{4352, 736});
  CHECK(det_input_size(960, 256, DetInputGeometry{}) ==
        sublift::adapters::paddle::detail::DetInputSize{2752, 736});
}

TEST_CASE(
    "Paddle Det geometry supports a pixel budget without downscaling",
    "[paddle][det][preprocess]") {
  using sublift::adapters::paddle::DetInputGeometry;
  using sublift::adapters::paddle::detail::det_input_size;

  const DetInputGeometry budget{736.0, 700000.0};
  CHECK(det_input_size(1920, 324, budget) ==
        sublift::adapters::paddle::detail::DetInputSize{2048, 352});
  CHECK(det_input_size(960, 256, budget) ==
        sublift::adapters::paddle::detail::DetInputSize{1632, 448});

  const DetInputGeometry smaller_than_native{736.0, 400000.0};
  CHECK(det_input_size(1920, 324, smaller_than_native) ==
        sublift::adapters::paddle::detail::DetInputSize{1920, 320});

  const DetInputGeometry native{0.0, 0.0};
  CHECK(det_input_size(1920, 324, native) ==
        sublift::adapters::paddle::detail::DetInputSize{1920, 320});
}

TEST_CASE(
    "Paddle Det geometry applies the conditional short-side floor",
    "[paddle][det][preprocess]") {
  using sublift::adapters::paddle::DetInputGeometry;
  using sublift::adapters::paddle::detail::det_input_size;

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

  const auto det = sublift::adapters::paddle::detail::prepare_det_input(
      rgb.data(), kWidth, kHeight,
      sublift::adapters::paddle::DetInputGeometry{736.0, 700000.0});
  CHECK(det.width == 1632);
  CHECK(det.height == 448);
  CHECK(det.nchw.size() == 3U * 1632U * 448U);
}

TEST_CASE(
    "Paddle global preprocess applies strip-adaptive 32-aligned vertical padding",
    "[paddle][det][preprocess]") {
  constexpr std::int32_t kWidth = 100;
  constexpr std::int32_t kHeight = 10;
  constexpr std::int32_t kStride = kWidth * 3 + 7;
  std::vector<std::uint8_t> strided(
      static_cast<std::size_t>(kStride) * kHeight, 255);

  const auto global = sublift::adapters::paddle::detail::prepare_global_image(
      strided.data(), kWidth, kHeight, kStride);
  // 100x10 has cols >= 30 and max <= 2000, so width is preserved without
  // destructive 32-multiple pre-resize; height is padded to 64 (floor_height
  // for cols < 512) with padding_top = (64 - 10) / 2 = 27.
  CHECK(global.width == 100);
  CHECK(global.height == 64);
  CHECK(global.padding_top == 27);
  CHECK(global.padding_left == 0);
  CHECK(global.ratio_h == Approx(1.0));
  CHECK(global.ratio_w == Approx(1.0));
  REQUIRE(global.rgb.size() == 100U * 64U * 3U);
  CHECK(global.rgb.front() == 0);
  CHECK(global.rgb[27U * 100U * 3U] == 255);
  CHECK(global.rgb[36U * 100U * 3U] == 255);
  CHECK(global.rgb[37U * 100U * 3U] == 0);
}

TEST_CASE(
    "Paddle strip-aware geometry and global preprocess handle UI ROI strips and 4K thin strips",
    "[paddle][det][preprocess][strip]") {
  using sublift::adapters::paddle::DetInputGeometry;
  using sublift::adapters::paddle::detail::DetInputSize;
  using sublift::adapters::paddle::detail::det_input_size;
  using sublift::adapters::paddle::detail::prepare_global_image;

  const DetInputGeometry budget600k{736.0, 600000.0};

  SECTION("1080p UI single-line and two-line strips avoid w/4 480px canvas inflation") {
    std::vector<std::uint8_t> roi87(1920U * 87U * 3U, 200U);
    const auto global87 = prepare_global_image(roi87.data(), 1920, 87, 1920 * 3);
    CHECK(global87.width == 1920);
    CHECK(global87.height == 128);
    CHECK(global87.padding_top == 20);
    CHECK(global87.ratio_w == Approx(1.0));
    CHECK(global87.ratio_h == Approx(1.0));
    CHECK(det_input_size(global87.width, global87.height, budget600k) ==
          DetInputSize{1920, 128});

    std::vector<std::uint8_t> roi54(1920U * 54U * 3U, 200U);
    const auto global54 = prepare_global_image(roi54.data(), 1920, 54, 1920 * 3);
    CHECK(global54.width == 1920);
    CHECK(global54.height == 96);
    CHECK(global54.padding_top == 21);
    CHECK(det_input_size(global54.width, global54.height, budget600k) ==
          DetInputSize{1920, 96});
  }

  SECTION("720p and 480p UI strips get consistent horizontal upscale without black-padding tax") {
    std::vector<std::uint8_t> roi720(1280U * 58U * 3U, 180U);
    const auto global720 = prepare_global_image(roi720.data(), 1280, 58, 1280 * 3);
    CHECK(global720.width == 1280);
    CHECK(global720.height == 80);
    CHECK(global720.padding_top == 11);
    const auto det720 = det_input_size(global720.width, global720.height, budget600k);
    CHECK(det720 == DetInputSize{1888, 128});

    std::vector<std::uint8_t> roi480(854U * 38U * 3U, 180U);
    const auto global480 = prepare_global_image(roi480.data(), 854, 38, 854 * 3);
    CHECK(global480.width == 854);
    CHECK(global480.height == 64);
    CHECK(global480.padding_top == 13);
    const auto det480 = det_input_size(global480.width, global480.height, budget600k);
    CHECK(det480 == DetInputSize{1888, 128});

    std::vector<std::uint8_t> roi480_thin(854U * 24U * 3U, 180U);
    const auto global480_thin =
        prepare_global_image(roi480_thin.data(), 854, 24, 854 * 3);
    CHECK(global480_thin.width == 854);
    CHECK(global480_thin.height == 64);
    CHECK(global480_thin.ratio_w == Approx(1.0));
    CHECK(global480_thin.ratio_h == Approx(1.0));
  }

  SECTION("4K ultra-thin strip 3840x28 downscales proportionally without zero-height crash") {
    std::vector<std::uint8_t> roi4k(3840U * 28U * 3U, 128U);
    const auto global4k = prepare_global_image(roi4k.data(), 3840, 28, 3840 * 3);
    CHECK(global4k.width == 2000);
    CHECK(global4k.height == 96);
    CHECK(global4k.ratio_w == Approx(3840.0 / 2000.0));
    CHECK(global4k.ratio_h == Approx(28.0 / 15.0));
    const auto det4k = det_input_size(global4k.width, global4k.height, budget600k);
    CHECK(det4k.width == 1984);
    CHECK(det4k.height == 96);
  }

  SECTION("min_side=736 rollback and short_side floor do not explode narrow w/h > 8 strips") {
    const DetInputGeometry min736{736.0, 0.0};
    const DetInputGeometry budget_floor352{736.0, 600000.0, 352.0};

    // Without the w/4 effective_short_side cap, 1920x96 under min_side=736
    // would explode to 14720x736 (10.83 MP). With the cap, horizontal scale
    // matches legacy RapidOCR w/4 (736/480 = 1.533x -> 2944) while keeping
    // compact vertical height (160).
    CHECK(det_input_size(1920, 96, min736) == DetInputSize{2944, 160});
    CHECK(det_input_size(1920, 128, min736) == DetInputSize{2944, 192});
    CHECK(det_input_size(2000, 96, min736) == DetInputSize{2944, 128});
    CHECK(det_input_size(1280, 80, min736) == DetInputSize{2944, 192});

    // Under pixel_budget=600000 with short_side=352, 1920x96 has effective
    // short side 480 >= 352 so it stays at 1920x96 instead of 7040x352.
    CHECK(det_input_size(1920, 96, budget_floor352) == DetInputSize{1920, 96});
    CHECK(det_input_size(1920, 128, budget_floor352) == DetInputSize{1920, 128});
    CHECK(det_input_size(1280, 80, budget_floor352) == DetInputSize{1888, 128});
  }

  SECTION("prepare_global_image_into reuses buffer capacity with byte-identical output") {
    sublift::adapters::paddle::detail::GlobalPreprocessResult reused;
    std::vector<std::uint8_t> roi_large(1920U * 324U * 3U, 99U);
    sublift::adapters::paddle::detail::prepare_global_image_into(
        roi_large.data(), 1920, 324, 1920 * 3, &reused);
    const auto* initial_ptr = reused.rgb.data();
    const auto initial_cap = reused.rgb.capacity();
    REQUIRE(reused.width == 1920);
    REQUIRE(reused.height == 324);

    std::vector<std::uint8_t> roi_strip(1920U * 84U * 3U, 177U);
    sublift::adapters::paddle::detail::prepare_global_image_into(
        roi_strip.data(), 1920, 84, 1920 * 3, &reused);
    const auto fresh = prepare_global_image(roi_strip.data(), 1920, 84, 1920 * 3);
    CHECK(reused.rgb.data() == initial_ptr);
    CHECK(reused.rgb.capacity() == initial_cap);
    CHECK(reused.width == fresh.width);
    CHECK(reused.height == fresh.height);
    CHECK(reused.padding_top == fresh.padding_top);
    CHECK(reused.padding_left == fresh.padding_left);
    CHECK(reused.ratio_w == Approx(fresh.ratio_w));
    CHECK(reused.ratio_h == Approx(fresh.ratio_h));
    CHECK(reused.rgb == fresh.rgb);
  }

  SECTION("Horizontally narrowed 1080p/720p subtitle boxes preserve reference_width scale and context padding") {
    // 1080p subtitle strip narrowed horizontally (e.g. RegionDetector inset to
    // exclude corner watermarks: 1344x68 or manual 560x84 box where w/h in
    // (4.0, 8.0] and rows < 140).
    std::vector<std::uint8_t> roi1080_inset(1344U * 68U * 3U, 190U);
    const auto global1080_inset =
        prepare_global_image(roi1080_inset.data(), 1344, 68, 1344 * 3);
    CHECK(global1080_inset.width == 1344);
    CHECK(global1080_inset.height == 96);
    CHECK(global1080_inset.padding_top == 14);

    // With reference_width = 1920, a 1080p narrowed strip (1344x96, 1344x128,
    // or 960x96) is already at native 1080p character resolution and must stay
    // at 1:1 instead of over-upscaling 1.41x-1.98x or stretching vertically.
    const DetInputGeometry budget1080_ref{736.0, 600000.0, 0.0, 1920.0};
    CHECK(det_input_size(global1080_inset.width, global1080_inset.height,
                         budget1080_ref) == DetInputSize{1344, 96});
    CHECK(det_input_size(960, 96, budget1080_ref) == DetInputSize{960, 96});

    std::vector<std::uint8_t> roi1080_inset84(1344U * 84U * 3U, 190U);
    const auto global1080_inset84 =
        prepare_global_image(roi1080_inset84.data(), 1344, 84, 1344 * 3);
    CHECK(global1080_inset84.width == 1344);
    CHECK(global1080_inset84.height == 128);
    CHECK(global1080_inset84.padding_top == 22);
    CHECK(det_input_size(global1080_inset84.width, global1080_inset84.height,
                         budget1080_ref) == DetInputSize{1344, 128});

    // Horizontally narrowed box with w/h in (4.0, 8.0] and h < 140 (560x84,
    // w/h = 6.67) still receives strip-adaptive vertical context padding.
    std::vector<std::uint8_t> roi1080_narrow_w(560U * 84U * 3U, 190U);
    const auto global1080_narrow_w =
        prepare_global_image(roi1080_narrow_w.data(), 560, 84, 560 * 3);
    CHECK(global1080_narrow_w.width == 560);
    CHECK(global1080_narrow_w.height == 112);
    CHECK(global1080_narrow_w.padding_top == 14);

    // 720p horizontally narrowed strip (896x58 on 1280x720 source) gets the
    // exact same 1.48x scale factor as a full-width 1280x58 strip instead of
    // over-upscaling 2.12x.
    std::vector<std::uint8_t> roi720_inset(896U * 58U * 3U, 180U);
    const auto global720_inset =
        prepare_global_image(roi720_inset.data(), 896, 58, 896 * 3);
    CHECK(global720_inset.width == 896);
    CHECK(global720_inset.height == 80);
    const DetInputGeometry budget720_ref{736.0, 600000.0, 0.0, 1280.0};
    CHECK(det_input_size(global720_inset.width, global720_inset.height,
                         budget720_ref) == DetInputSize{1344, 128});
  }
}

namespace {

std::vector<float> reference_det_nchw(const std::vector<std::uint8_t>& rgb,
                                      std::int32_t width,
                                      std::int32_t height,
                                      const sublift::adapters::paddle::DetInputGeometry& geometry,
                                      int* out_width,
                                      int* out_height) {
  const auto size = sublift::adapters::paddle::detail::det_input_size(width, height, geometry);
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
  const auto global = sublift::adapters::paddle::detail::prepare_global_image(
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
    sublift::adapters::paddle::DetInputGeometry geometry;
  };
  const std::array<Case, 6> cases{{
      {320, 96, sublift::adapters::paddle::DetInputGeometry{736.0, 600000.0}},
      {1920, 324, sublift::adapters::paddle::DetInputGeometry{736.0, 600000.0}},
      {1920, 128, sublift::adapters::paddle::DetInputGeometry{736.0, 600000.0}},
      {641, 217, sublift::adapters::paddle::DetInputGeometry{736.0, 600000.0}},
      {960, 256, sublift::adapters::paddle::DetInputGeometry{736.0, 700000.0}},
      {1280, 216, sublift::adapters::paddle::DetInputGeometry{736.0, 600000.0, 352.0}},
  }};
  for (const auto& test_case : cases) {
    std::vector<std::uint8_t> rgb(
        static_cast<std::size_t>(test_case.width) *
        static_cast<std::size_t>(test_case.height) * 3U);
    for (auto& value : rgb) {
      value = static_cast<std::uint8_t>(pixel(rng));
    }
    const auto actual = sublift::adapters::paddle::detail::prepare_det_input(
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
