#include <catch2/catch_test_macros.hpp>
#include <vector>

#include "sublift/paddle_geometry.hpp"

TEST_CASE("Paddle Geometry - quad_to_aabb", "[paddle][geometry]") {
  SECTION("Standard box with fractional coordinates") {
    sublift::QuadCorners corners = {
        sublift::Point2D{10.2f, 20.8f},
        sublift::Point2D{100.7f, 20.1f},
        sublift::Point2D{100.9f, 60.4f},
        sublift::Point2D{10.1f, 60.9f},
    };
    sublift::OcrCropBox box = sublift::quad_to_aabb(corners);

    // x_min = floor(10.1) = 10
    // y_min = floor(20.1) = 20
    // x_max = ceil(100.9) = 101
    // y_max = ceil(60.9) = 61
    // width = 101 - 10 = 91
    // height = 61 - 20 = 41
    REQUIRE(box.x == 10);
    REQUIRE(box.y == 20);
    REQUIRE(box.width == 91);
    REQUIRE(box.height == 41);
  }

  SECTION("Degenerate point quad") {
    sublift::QuadCorners corners = {
        sublift::Point2D{15.0f, 25.0f},
        sublift::Point2D{15.0f, 25.0f},
        sublift::Point2D{15.0f, 25.0f},
        sublift::Point2D{15.0f, 25.0f},
    };
    sublift::OcrCropBox box = sublift::quad_to_aabb(corners);
    REQUIRE(box.x == 15);
    REQUIRE(box.y == 25);
    REQUIRE(box.width == 0);
    REQUIRE(box.height == 0);
  }
}

TEST_CASE("Paddle Geometry - clamp_box", "[paddle][geometry]") {
  SECTION("Normal inside box") {
    sublift::OcrCropBox box{10, 10, 50, 50};
    sublift::OcrCropBox clamped = sublift::clamp_box(box, 100, 100);
    REQUIRE(clamped.x == 10);
    REQUIRE(clamped.y == 10);
    REQUIRE(clamped.width == 50);
    REQUIRE(clamped.height == 50);
  }

  SECTION("Out-of-bounds box") {
    sublift::OcrCropBox box{-10, -5, 120, 150};
    sublift::OcrCropBox clamped = sublift::clamp_box(box, 100, 100);
    REQUIRE(clamped.x == 0);
    REQUIRE(clamped.y == 0);
    REQUIRE(clamped.width == 100);
    REQUIRE(clamped.height == 100);
  }
}

TEST_CASE("Paddle Geometry - sort_ocr_lines", "[paddle][geometry]") {
  std::vector<sublift::OcrLine> lines = {
      sublift::OcrLine{"line2", 0.9, sublift::OcrCropBox{10, 50, 100, 20}},
      sublift::OcrLine{"line1_right", 0.95, sublift::OcrCropBox{50, 10, 100, 20}},
      sublift::OcrLine{"line1_left", 0.8, sublift::OcrCropBox{10, 10, 100, 20}},
  };

  sublift::sort_ocr_lines(lines);

  REQUIRE(lines[0].text == "line1_left");
  REQUIRE(lines[1].text == "line1_right");
  REQUIRE(lines[2].text == "line2");
}

TEST_CASE("Paddle Geometry - is_strip_empty", "[paddle][geometry]") {
  REQUIRE(sublift::is_strip_empty(""));
  REQUIRE(sublift::is_strip_empty("   \t\r\n"));
  REQUIRE_FALSE(sublift::is_strip_empty("  hello  "));
  REQUIRE_FALSE(sublift::is_strip_empty("sublift"));
}

TEST_CASE("Paddle Geometry - rgb24_to_bgr24", "[paddle][geometry]") {
  SECTION("Compact 2x1 image") {
    // RGB pixels: Pixel0 = (255, 0, 128), Pixel1 = (10, 20, 30)
    std::vector<uint8_t> rgb = {255, 0, 128, 10, 20, 30};
    std::vector<uint8_t> bgr(6, 0);

    sublift::rgb24_to_bgr24(rgb.data(), bgr.data(), 2, 1);

    // Expect BGR: Pixel0 = (128, 0, 255), Pixel1 = (30, 20, 10)
    REQUIRE(bgr[0] == 128);
    REQUIRE(bgr[1] == 0);
    REQUIRE(bgr[2] == 255);
    REQUIRE(bgr[3] == 30);
    REQUIRE(bgr[4] == 20);
    REQUIRE(bgr[5] == 10);
  }

  SECTION("Image with stride padding") {
    // 2x1 RGB with src_stride = 8, dst_stride = 8
    std::vector<uint8_t> rgb = {255, 0, 128, 10, 20, 30, 0, 0};
    std::vector<uint8_t> bgr(8, 0);

    sublift::rgb24_to_bgr24(rgb.data(), bgr.data(), 2, 1, 8, 8);

    REQUIRE(bgr[0] == 128);
    REQUIRE(bgr[1] == 0);
    REQUIRE(bgr[2] == 255);
    REQUIRE(bgr[3] == 30);
    REQUIRE(bgr[4] == 20);
    REQUIRE(bgr[5] == 10);
  }
}
