#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <cmath>

#include "sublift/models.hpp"
#include "sublift/paddle_geometry.hpp"

#ifndef SUBLIFT_PARITY_GOLDEN_PADDLE
#define SUBLIFT_PARITY_GOLDEN_PADDLE ""
#endif

TEST_CASE("Paddle Parity - Golden JSON Verification", "[paddle][parity]") {
  std::string golden_path = SUBLIFT_PARITY_GOLDEN_PADDLE;
  if (golden_path.empty()) {
    SKIP("SUBLIFT_PARITY_GOLDEN_PADDLE is not set");
  }

  std::ifstream f(golden_path);
  REQUIRE(f.is_open());

  nlohmann::json j = nlohmann::json::parse(f);
  REQUIRE(j.contains("lines"));
  REQUIRE(j.contains("valid_model_types"));
  REQUIRE(j.contains("text"));
  REQUIRE(j.contains("confidence"));

  std::vector<sublift::OcrLine> test_lines = {
      sublift::OcrLine{"  line2_bottom  ", 0.88, sublift::OcrCropBox{12, 60, 120, 30}},
      sublift::OcrLine{"line1_right", 0.95, sublift::OcrCropBox{60, 15, 80, 25}},
      sublift::OcrLine{"line1_left", 0.91, sublift::OcrCropBox{10, 15, 45, 25}},
      sublift::OcrLine{"   \t\n", 0.50, sublift::OcrCropBox{0, 0, 10, 10}},
  };

  // Filter empty strip
  std::vector<sublift::OcrLine> filtered;
  for (auto& line : test_lines) {
    if (!sublift::is_strip_empty(line.text)) {
      filtered.push_back(line);
    }
  }

  // Sort (y, x)
  sublift::sort_ocr_lines(filtered);

  sublift::OcrResult res = sublift::OcrResult::from_lines(filtered);

  // 1. Overall result assertions
  REQUIRE(res.text == j["text"].get<std::string>());
  REQUIRE(res.confidence == Catch::Approx(j["confidence"].get<double>()).margin(1e-4));

  // 2. Lines assertions
  REQUIRE(res.lines.size() == j["lines"].size());
  for (size_t i = 0; i < res.lines.size(); ++i) {
    REQUIRE(res.lines[i].text == j["lines"][i]["text"].get<std::string>());
    REQUIRE(res.lines[i].confidence == Catch::Approx(j["lines"][i]["confidence"].get<double>()).margin(1e-4));
    REQUIRE(res.lines[i].box.x == j["lines"][i]["box"]["x"].get<int32_t>());
    REQUIRE(res.lines[i].box.y == j["lines"][i]["box"]["y"].get<int32_t>());
    REQUIRE(res.lines[i].box.width == j["lines"][i]["box"]["width"].get<int32_t>());
    REQUIRE(res.lines[i].box.height == j["lines"][i]["box"]["height"].get<int32_t>());
  }
}
