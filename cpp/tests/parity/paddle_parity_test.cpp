#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>

#include <sublift/adapters/paddle/paddle_geometry.hpp>
#include <sublift/adapters/paddle/paddle_ocr.hpp>
#include <sublift/core/image_view.hpp>
#include <sublift/ports/ocr_engine.hpp>

#ifndef SUBLIFT_PROJECT_SOURCE_DIR
#define SUBLIFT_PROJECT_SOURCE_DIR ""
#endif

#ifndef SUBLIFT_PARITY_PADDLE_FIXTURES_DIR
#define SUBLIFT_PARITY_PADDLE_FIXTURES_DIR ""
#endif

#ifndef SUBLIFT_PARITY_PADDLE_MANIFEST
#define SUBLIFT_PARITY_PADDLE_MANIFEST ""
#endif

#ifndef SUBLIFT_PARITY_GOLDEN_PADDLE
#define SUBLIFT_PARITY_GOLDEN_PADDLE ""
#endif

namespace {

std::filesystem::path resolve_manifest_path() {
  if (std::string(SUBLIFT_PARITY_PADDLE_MANIFEST).size() > 0) {
    std::filesystem::path p{SUBLIFT_PARITY_PADDLE_MANIFEST};
    if (std::filesystem::exists(p)) {
      return p;
    }
  }

  if (std::string(SUBLIFT_PROJECT_SOURCE_DIR).size() > 0) {
    std::filesystem::path p =
        std::filesystem::path{SUBLIFT_PROJECT_SOURCE_DIR} / "benchmark/parity/fixtures/paddle/manifest.json";
    if (std::filesystem::exists(p)) {
      return p;
    }
  }

  std::filesystem::path cur = std::filesystem::current_path();
  for (int i = 0; i < 4; ++i) {
    std::filesystem::path p = cur / "benchmark/parity/fixtures/paddle/manifest.json";
    if (std::filesystem::exists(p)) {
      return p;
    }
    if (!cur.has_parent_path() || cur == cur.parent_path()) break;
    cur = cur.parent_path();
  }

  return {};
}

}  // namespace

TEST_CASE("Paddle Parity - Real Inference on 9 Fixtures", "[paddle][parity]") {
  if (!sublift::adapters::paddle::is_paddle_available()) {
    SKIP("Paddle model bundle is not installed or available on this machine");
  }

  const auto manifest_path = resolve_manifest_path();
  if (manifest_path.empty() || !std::filesystem::exists(manifest_path)) {
    SKIP("Paddle parity fixture manifest not found at: " + manifest_path.string());
  }

  const auto fixtures_dir = manifest_path.parent_path();

  std::ifstream f(manifest_path);
  REQUIRE(f.is_open());
  nlohmann::json manifest = nlohmann::json::parse(f);
  REQUIRE(manifest.contains("fixtures"));
  const auto& fixtures = manifest["fixtures"];
  REQUIRE_FALSE(fixtures.empty());

  sublift::adapters::paddle::PaddleOcrEngine engine;

  for (const auto& item : fixtures) {
    const std::string name = item["name"].get<std::string>();
    const int expected_w = item["width"].get<int>();
    const int expected_h = item["height"].get<int>();
    const auto expected_texts = item["expected_text"].get<std::vector<std::string>>();

    {
      UNSCOPED_INFO("Fixture: " << name);
      const auto img_path = fixtures_dir / name;
      REQUIRE(std::filesystem::exists(img_path));

      cv::Mat bgr = cv::imread(img_path.string(), cv::IMREAD_COLOR);
      REQUIRE_FALSE(bgr.empty());
      REQUIRE(bgr.cols == expected_w);
      REQUIRE(bgr.rows == expected_h);

      cv::Mat rgb;
      cv::cvtColor(bgr, rgb, cv::COLOR_BGR2RGB);

      sublift::ImageView view(
          rgb.data,
          rgb.cols,
          rgb.rows,
          static_cast<std::int32_t>(rgb.step),
          sublift::PixelFormat::RGB24);

      const sublift::OcrResult result = engine.recognize(view);

      REQUIRE(result.lines.size() == expected_texts.size());

      auto strip_spaces = [](std::string_view s) {
        std::string out;
        for (char c : s) {
          if (c != ' ' && c != '\t') out += c;
        }
        return out;
      };

      for (std::size_t i = 0; i < expected_texts.size(); ++i) {
        CHECK((result.lines[i].text == expected_texts[i] ||
               strip_spaces(result.lines[i].text) == strip_spaces(expected_texts[i])));
        CHECK(result.lines[i].confidence >= 0.50);
        CHECK(result.lines[i].box.width > 0);
        CHECK(result.lines[i].box.height > 0);
        CHECK(result.lines[i].box.x >= 0);
        CHECK(result.lines[i].box.y >= 0);
        CHECK(result.lines[i].box.x + result.lines[i].box.width <= expected_w);
        CHECK(result.lines[i].box.y + result.lines[i].box.height <= expected_h);
      }

      if (expected_texts.empty()) {
        CHECK(result.text.empty());
        CHECK(result.confidence == 0.0);
      } else {
        std::string expected_combined;
        for (std::size_t i = 0; i < expected_texts.size(); ++i) {
          if (i > 0) {
            expected_combined += "\n";
          }
          expected_combined += expected_texts[i];
        }
        CHECK((result.text == expected_combined ||
               strip_spaces(result.text) == strip_spaces(expected_combined)));
        CHECK(result.confidence >= 0.50);
      }
    }
  }
}

TEST_CASE("Paddle Parity - Golden JSON Verification", "[paddle][parity]") {
  std::string golden_path = SUBLIFT_PARITY_GOLDEN_PADDLE;
  if (golden_path.empty() || !std::filesystem::exists(golden_path)) {
    SKIP("SUBLIFT_PARITY_GOLDEN_PADDLE is not set or missing");
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
    if (!sublift::adapters::paddle::is_strip_empty(line.text)) {
      filtered.push_back(line);
    }
  }

  // Sort (y, x)
  sublift::adapters::paddle::sort_ocr_lines(filtered);

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
