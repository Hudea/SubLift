#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

#include "sublift/test_support.hpp"
#include "sublift/vision.hpp"

using namespace sublift;
using namespace sublift::test_support;

#ifndef SUBLIFT_PARITY_GOLDEN_VISION
#error "SUBLIFT_PARITY_GOLDEN_VISION macro is required for vision parity test"
#endif

TEST_CASE("Vision parity vs frozen golden", "[parity][vision]") {
  const auto golden = load_vision_golden(SUBLIFT_PARITY_GOLDEN_VISION);

  SECTION("default_recognition_languages parity") {
    REQUIRE(kDefaultVisionLanguages.size() == golden.default_languages.size());
    for (std::size_t i = 0; i < kDefaultVisionLanguages.size(); ++i) {
      REQUIRE(kDefaultVisionLanguages[i] == golden.default_languages[i]);
    }
  }

  SECTION("normalized_box_to_pixel parity") {
    for (const auto& tc : golden.box_cases) {
      INFO("box_case: " << tc.name);
      OcrCropBox actual = vision_normalized_box_to_pixel(
          tc.nx, tc.ny, tc.nw, tc.nh, tc.image_width, tc.image_height);
      REQUIRE(actual.x == tc.expected_box.x);
      REQUIRE(actual.y == tc.expected_box.y);
      REQUIRE(actual.width == tc.expected_box.width);
      REQUIRE(actual.height == tc.expected_box.height);
    }
  }

  SECTION("clamp_box parity") {
    for (const auto& tc : golden.clamp_cases) {
      INFO("clamp_case: " << tc.name);
      OcrCropBox actual = clamp_ocr_box(
          tc.x, tc.y, tc.w, tc.h, tc.image_width, tc.image_height);
      REQUIRE(actual.x == tc.expected_box.x);
      REQUIRE(actual.y == tc.expected_box.y);
      REQUIRE(actual.width == tc.expected_box.width);
      REQUIRE(actual.height == tc.expected_box.height);
    }
  }

  SECTION("line_sorting parity") {
    for (const auto& tc : golden.sort_cases) {
      INFO("sort_case: " << tc.name);
      std::vector<OcrLine> lines = tc.input_lines;
      sort_ocr_lines(lines);

      REQUIRE(lines.size() == tc.expected_lines.size());
      for (std::size_t i = 0; i < lines.size(); ++i) {
        REQUIRE(lines[i].text == tc.expected_lines[i].text);
        REQUIRE(lines[i].confidence == tc.expected_lines[i].confidence);
        REQUIRE(lines[i].box.x == tc.expected_lines[i].box.x);
        REQUIRE(lines[i].box.y == tc.expected_lines[i].box.y);
        REQUIRE(lines[i].box.width == tc.expected_lines[i].box.width);
        REQUIRE(lines[i].box.height == tc.expected_lines[i].box.height);
      }
    }
  }

  SECTION("empty_result structure L0") {
    REQUIRE_FALSE(golden.empty_result_cases.empty());
    for (const auto& tc : golden.empty_result_cases) {
      INFO("empty_result: " << tc.name);
      // perform-fail / no-obs / all-blank → default OcrResult{}
      const OcrResult actual{};
      REQUIRE(actual.text == tc.expected_text);
      REQUIRE(actual.confidence == tc.expected_confidence);
      REQUIRE(actual.lines.size() == tc.expected_line_count);
      // from_lines({}) must match the same frozen structure
      const OcrResult from_empty = OcrResult::from_lines({});
      REQUIRE(from_empty.text == tc.expected_text);
      REQUIRE(from_empty.confidence == tc.expected_confidence);
      REQUIRE(from_empty.lines.size() == tc.expected_line_count);
    }
  }
}
