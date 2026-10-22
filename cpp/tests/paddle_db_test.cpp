#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <random>
#include <vector>

#include <opencv2/imgproc.hpp>

#include "ppocr_db_postprocess.hpp"

using Catch::Approx;

namespace {

using sublift::paddle::QuadPolygon;

void check_quad(
    const QuadPolygon& actual,
    const std::array<std::array<float, 2>, 4>& expected,
    float tolerance = 0.0F) {
  const std::array<sublift::Point2D, 4> points{
      actual.p0, actual.p1, actual.p2, actual.p3};
  for (std::size_t index = 0; index < points.size(); ++index) {
    CHECK(points[index].x == Approx(expected[index][0]).margin(tolerance));
    CHECK(points[index].y == Approx(expected[index][1]).margin(tolerance));
  }
}

}  // namespace

TEST_CASE("DBPostProcess empty probability map returns 0 boxes", "[paddle][db]") {
  std::vector<float> probability(100 * 100, 0.0F);
  const auto result = sublift::paddle::db_postprocess(
      probability.data(), 100, 100, 100, 100);
  CHECK(result.quads.empty());
  CHECK(result.aabbs.empty());
}

TEST_CASE(
    "DBPostProcess matches RapidOCR rectangle score and unclip",
    "[paddle][db]") {
  constexpr int kWidth = 100;
  constexpr int kHeight = 100;
  std::vector<float> probability(kWidth * kHeight, 0.0F);
  for (int y = 30; y < 50; ++y) {
    for (int x = 20; x < 60; ++x) {
      probability[static_cast<std::size_t>(y) * kWidth + x] = 0.95F;
    }
  }

  const auto result = sublift::paddle::db_postprocess(
      probability.data(), kHeight, kWidth, kHeight, kWidth);
  REQUIRE(result.quads.size() == 1);
  check_quad(
      result.quads[0],
      {{{9.0F, 19.0F}, {71.0F, 19.0F}, {71.0F, 61.0F}, {9.0F, 61.0F}}},
      1.0F);
  CHECK(result.quads[0].score == Approx(0.8826945302F).margin(1e-4F));
}

TEST_CASE(
    "DBPostProcess and TextDetector sort match frozen RapidOCR semantics",
    "[paddle][db]") {
  constexpr int kWidth = 100;
  constexpr int kHeight = 100;
  std::vector<float> probability(kWidth * kHeight, 0.0F);
  for (int y = 10; y < 20; ++y) {
    for (int x = 10; x < 30; ++x) {
      probability[static_cast<std::size_t>(y) * kWidth + x] = 0.8F;
    }
  }
  for (int y = 70; y < 90; ++y) {
    for (int x = 60; x < 90; ++x) {
      probability[static_cast<std::size_t>(y) * kWidth + x] = 0.9F;
    }
  }

  auto result = sublift::paddle::db_postprocess(
      probability.data(), kHeight, kWidth, kHeight, kWidth);
  REQUIRE(result.quads.size() == 2);
  // cv::findContours returns the lower contour first, as does Python.
  check_quad(
      result.quads[0],
      {{{50.0F, 60.0F}, {99.0F, 60.0F}, {99.0F, 99.0F}, {50.0F, 99.0F}}},
      1.0F);
  check_quad(
      result.quads[1],
      {{{5.0F, 5.0F}, {35.0F, 5.0F}, {35.0F, 25.0F}, {5.0F, 25.0F}}},
      1.0F);
  CHECK(result.quads[0].score == Approx(0.8294930656F).margin(1e-4F));
  CHECK(result.quads[1].score == Approx(0.6926407030F).margin(1e-4F));

  sublift::paddle::sort_db_result(&result);
  REQUIRE(result.quads.size() == 2);
  CHECK(result.quads[0].p0.y <= result.quads[1].p0.y);
  // RapidOCR sorts only coordinates; its score array remains positional.
  CHECK(result.quads[0].score == Approx(0.8294930656F).margin(1e-4F));
  CHECK(result.quads[1].score == Approx(0.6926407030F).margin(1e-4F));
}

TEST_CASE(
    "DBPostProcess threshold is strict and rejects undersized contours",
    "[paddle][db]") {
  constexpr int kSize = 32;
  std::vector<float> probability(kSize * kSize, 0.0F);
  for (int y = 10; y < 20; ++y) {
    for (int x = 10; x < 20; ++x) {
      probability[static_cast<std::size_t>(y) * kSize + x] = 0.3F;
    }
  }
  auto result = sublift::paddle::db_postprocess(
      probability.data(), kSize, kSize, kSize, kSize);
  CHECK(result.quads.empty());

  std::fill(probability.begin(), probability.end(), 0.0F);
  for (int y = 10; y < 12; ++y) {
    for (int x = 10; x < 12; ++x) {
      probability[static_cast<std::size_t>(y) * kSize + x] = 0.9F;
    }
  }
  result = sublift::paddle::db_postprocess(
      probability.data(), kSize, kSize, kSize, kSize);
  CHECK(result.quads.empty());
}

TEST_CASE(
    "DBPostProcess round unclip matches pyclipper for a rotated contour",
    "[paddle][db][clipper]") {
  constexpr int kWidth = 128;
  constexpr int kHeight = 128;
  std::vector<float> probability(kWidth * kHeight, 0.0F);
  cv::Mat probability_mat(
      kHeight, kWidth, CV_32FC1, probability.data());
  const std::array<cv::Point, 4> polygon{
      cv::Point{30, 78},
      cv::Point{28, 58},
      cv::Point{97, 49},
      cv::Point{99, 69},
  };
  const cv::Point* polygons[] = {polygon.data()};
  const int polygon_sizes[] = {static_cast<int>(polygon.size())};
  cv::fillPoly(
      probability_mat,
      polygons,
      polygon_sizes,
      1,
      cv::Scalar(0.95F));

  const auto result = sublift::paddle::db_postprocess(
      probability.data(), kHeight, kWidth, kHeight, kWidth);
  REQUIRE(result.quads.size() == 1);
  check_quad(
      result.quads[0],
      {{{11.0F, 46.0F}, {110.0F, 33.0F},
        {116.0F, 81.0F}, {17.0F, 94.0F}}});
  CHECK(result.quads[0].score ==
        Approx(0.8286049134F).margin(1e-5F));
}

TEST_CASE("DBPostProcess parallel candidates match the serial reference",
          "[paddle][db][equivalence]") {
  using sublift::paddle::DBPostProcessOptions;
  std::mt19937 rng(20261022U);
  std::uniform_real_distribution<float> noise(0.0F, 1.0F);
  for (int iteration = 0; iteration < 12; ++iteration) {
    const int width = 64 + static_cast<int>(rng() % 192U);
    const int height = 32 + static_cast<int>(rng() % 128U);
    std::vector<float> probability(
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height),
        0.0F);
    for (auto& value : probability) {
      value = noise(rng) * 0.6F;
    }
    const int boxes = 1 + static_cast<int>(rng() % 6U);
    for (int box = 0; box < boxes; ++box) {
      const int x0 = static_cast<int>(rng() % static_cast<unsigned>(width - 16));
      const int y0 = static_cast<int>(rng() % static_cast<unsigned>(height - 16));
      const int box_width = 4 + static_cast<int>(rng() % 24U);
      const int box_height = 3 + static_cast<int>(rng() % 10U);
      for (int y = y0; y < std::min(height, y0 + box_height); ++y) {
        for (int x = x0; x < std::min(width, x0 + box_width); ++x) {
          probability[static_cast<std::size_t>(y) * width + x] =
              0.8F + noise(rng) * 0.2F;
        }
      }
    }

    DBPostProcessOptions serial_options;
    serial_options.parallel = false;
    DBPostProcessOptions parallel_options;
    parallel_options.parallel = true;
    const auto serial = sublift::paddle::db_postprocess(
        probability.data(), height, width, height * 2, width * 2,
        serial_options);
    const auto parallel = sublift::paddle::db_postprocess(
        probability.data(), height, width, height * 2, width * 2,
        parallel_options);

    INFO("iteration=" << iteration << " map=" << width << "x" << height);
    REQUIRE(serial.quads.size() == parallel.quads.size());
    REQUIRE(serial.aabbs.size() == parallel.aabbs.size());
    for (std::size_t index = 0; index < serial.quads.size(); ++index) {
      const auto& lhs = serial.quads[index];
      const auto& rhs = parallel.quads[index];
      CHECK(lhs.score == rhs.score);
      CHECK(lhs.p0.x == rhs.p0.x);
      CHECK(lhs.p0.y == rhs.p0.y);
      CHECK(lhs.p1.x == rhs.p1.x);
      CHECK(lhs.p1.y == rhs.p1.y);
      CHECK(lhs.p2.x == rhs.p2.x);
      CHECK(lhs.p2.y == rhs.p2.y);
      CHECK(lhs.p3.x == rhs.p3.x);
      CHECK(lhs.p3.y == rhs.p3.y);
      CHECK(serial.aabbs[index].x == parallel.aabbs[index].x);
      CHECK(serial.aabbs[index].y == parallel.aabbs[index].y);
      CHECK(serial.aabbs[index].width == parallel.aabbs[index].width);
      CHECK(serial.aabbs[index].height == parallel.aabbs[index].height);
    }
  }
}
