#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include <sublift/adapters/apple_vision/apple_vision_geometry.hpp>
#include <sublift/adapters/apple_vision/apple_vision_ocr.hpp>
#include <sublift/adapters/apple_vision/apple_vision_stage_timing.hpp>

using namespace sublift;
using namespace sublift::adapters::apple_vision;

TEST_CASE("AppleVision geometry: bankers_round", "[apple_vision][unit]") {
  SECTION("round half to even matching Python 3 round()") {
    REQUIRE(bankers_round(0.5) == 0);
    REQUIRE(bankers_round(1.5) == 2);
    REQUIRE(bankers_round(2.5) == 2);
    REQUIRE(bankers_round(3.5) == 4);
    REQUIRE(bankers_round(-0.5) == 0);
    REQUIRE(bankers_round(-1.5) == -2);
    REQUIRE(bankers_round(-2.5) == -2);
  }

  SECTION("non-half rounding") {
    REQUIRE(bankers_round(0.4) == 0);
    REQUIRE(bankers_round(0.6) == 1);
    REQUIRE(bankers_round(1.4) == 1);
    REQUIRE(bankers_round(1.6) == 2);
    REQUIRE(bankers_round(-0.4) == 0);
    REQUIRE(bankers_round(-0.6) == -1);
    REQUIRE(bankers_round(-1.4) == -1);
    REQUIRE(bankers_round(-1.6) == -2);
    REQUIRE(bankers_round(-3.5) == -4);
  }

  SECTION("non-finite and out-of-range inputs are safely guarded") {
    REQUIRE(bankers_round(std::numeric_limits<double>::quiet_NaN()) == 0);
    REQUIRE(bankers_round(std::numeric_limits<double>::infinity()) == 0);
    REQUIRE(bankers_round(-std::numeric_limits<double>::infinity()) == 0);
    REQUIRE(bankers_round(1e20) == std::numeric_limits<std::int32_t>::max());
    REQUIRE(bankers_round(-1e20) == std::numeric_limits<std::int32_t>::min());
    REQUIRE(bankers_round(static_cast<double>(std::numeric_limits<std::int32_t>::max()) + 100.0) ==
            std::numeric_limits<std::int32_t>::max());
    REQUIRE(bankers_round(static_cast<double>(std::numeric_limits<std::int32_t>::min()) - 100.0) ==
            std::numeric_limits<std::int32_t>::min());
  }
}

TEST_CASE("AppleVision geometry: clamp_ocr_box", "[apple_vision][unit]") {
  REQUIRE(clamp_ocr_box(10, 20, 100, 50, 0, 1080) == OcrCropBox{0, 0, 0, 0});
  REQUIRE(clamp_ocr_box(10, 20, 100, 50, 1920, 0) == OcrCropBox{0, 0, 0, 0});
  REQUIRE(clamp_ocr_box(10, 20, 100, 50, -100, -100) == OcrCropBox{0, 0, 0, 0});
  REQUIRE(clamp_ocr_box(10, 20, 100, 50, 1920, 1080) == OcrCropBox{10, 20, 100, 50});
  REQUIRE(clamp_ocr_box(-10, -5, 100, 50, 1920, 1080) == OcrCropBox{0, 0, 100, 50});
  REQUIRE(clamp_ocr_box(1900, 1050, 100, 100, 1920, 1080) == OcrCropBox{1900, 1050, 20, 30});
  REQUIRE(clamp_ocr_box(2000, 500, 50, 50, 1920, 1080) == OcrCropBox{1920, 500, 0, 50});
  REQUIRE(clamp_ocr_box(500, 1200, 50, 50, 1920, 1080) == OcrCropBox{500, 1080, 50, 0});
  REQUIRE(clamp_ocr_box(0, 0, 1920, 1080, 1920, 1080) == OcrCropBox{0, 0, 1920, 1080});
}

TEST_CASE("AppleVision geometry: vision_normalized_box_to_pixel", "[apple_vision][unit]") {
  REQUIRE(vision_normalized_box_to_pixel(0.0, 0.0, 1.0, 1.0, 1920, 1080) ==
          OcrCropBox{0, 0, 1920, 1080});
  REQUIRE(vision_normalized_box_to_pixel(0.0, 0.5, 0.5, 0.5, 1920, 1080) ==
          OcrCropBox{0, 0, 960, 540});
  REQUIRE(vision_normalized_box_to_pixel(0.5, 0.0, 0.5, 0.5, 1920, 1080) ==
          OcrCropBox{960, 540, 960, 540});
  REQUIRE(vision_normalized_box_to_pixel(0.1, 0.2, 0.3, 0.4, 1000, 1000) ==
          OcrCropBox{100, 400, 300, 400});
  REQUIRE(vision_normalized_box_to_pixel(-0.1, 1.2, 0.5, 0.5, 1000, 1000) ==
          OcrCropBox{0, 0, 500, 500});
  REQUIRE(vision_normalized_box_to_pixel(0.0005, 0.0, 0.0015, 0.0025, 1000, 1000) ==
          OcrCropBox{0, 998, 2, 2});
  REQUIRE(vision_normalized_box_to_pixel(0.1, 0.1, 0.5, 0.5, 0, 1000) ==
          OcrCropBox{0, 0, 0, 0});
  REQUIRE(vision_normalized_box_to_pixel(0.1, 0.1, 0.5, 0.5, 1000, -1) ==
          OcrCropBox{0, 0, 0, 0});
}

TEST_CASE("AppleVision geometry: sort_ocr_lines & is_ocr_text_blank", "[apple_vision][unit]") {
  SECTION("sort_ocr_lines orders by y then x stably") {
    std::vector<OcrLine> lines = {
        OcrLine{"line1", 0.8, OcrCropBox{100, 50, 40, 10}},
        OcrLine{"line2", 0.9, OcrCropBox{20, 10, 40, 10}},
        OcrLine{"line3", 0.95, OcrCropBox{10, 50, 40, 10}},
        OcrLine{"line4_tie", 0.99, OcrCropBox{10, 50, 40, 10}},
    };
    sort_ocr_lines(lines);
    REQUIRE(lines[0].text == "line2");
    REQUIRE(lines[1].text == "line3");
    REQUIRE(lines[2].text == "line4_tie");
    REQUIRE(lines[3].text == "line1");
  }

  SECTION("is_ocr_text_blank checks ASCII and Unicode whitespace") {
    REQUIRE(is_ocr_text_blank(""));
    REQUIRE(is_ocr_text_blank("  \t\n\r\f\v"));
    REQUIRE(is_ocr_text_blank("\xc2\xa0"));      // U+00A0 NBSP
    REQUIRE(is_ocr_text_blank("\xe3\x80\x80"));  // U+3000 ideographic space
    REQUIRE(is_ocr_text_blank("\xe2\x80\x83"));  // U+2003 EM SPACE
    REQUIRE(is_ocr_text_blank(" \xe3\x80\x80 \t"));
    REQUIRE_FALSE(is_ocr_text_blank("a"));
    REQUIRE_FALSE(is_ocr_text_blank("  hi  "));
    REQUIRE_FALSE(is_ocr_text_blank("\xe3\x80\x80x"));
    // 4-byte UTF-8 emoji and truncated UTF-8 lead bytes must not OOB read or report blank
    REQUIRE_FALSE(is_ocr_text_blank("\xf0\x9f\x98\x80"));
    REQUIRE_FALSE(is_ocr_text_blank("\xc2"));
    REQUIRE_FALSE(is_ocr_text_blank("\xe3\x80"));
    REQUIRE_FALSE(is_ocr_text_blank(" \xc2"));
  }

  SECTION("filtering blank lines + OcrResult::from_lines") {
    std::vector<OcrLine> raw = {
        OcrLine{"  ", 0.9, OcrCropBox{0, 0, 50, 50}},
        OcrLine{"\xe3\x80\x80", 0.8, OcrCropBox{0, 0, 50, 50}},
    };
    std::vector<OcrLine> kept;
    for (const auto& line : raw) {
      if (!is_ocr_text_blank(line.text)) {
        kept.push_back(line);
      }
    }
    const OcrResult empty_res = OcrResult::from_lines(kept);
    REQUIRE(empty_res.text.empty());
    REQUIRE(empty_res.confidence == 0.0);
    REQUIRE(empty_res.lines.empty());

    std::vector<OcrLine> two = {
        OcrLine{"bottom", 0.5, OcrCropBox{0, 70, 100, 30}},
        OcrLine{"top", 0.9, OcrCropBox{0, 0, 100, 30}},
    };
    sort_ocr_lines(two);
    const OcrResult joined = OcrResult::from_lines(two);
    REQUIRE(joined.text == "top\nbottom");
    REQUIRE(joined.confidence == Catch::Approx(0.7));
  }
}

TEST_CASE("AppleVision options, availability and contract", "[apple_vision][unit]") {
  REQUIRE(kDefaultVisionLanguages.size() == 2);
  REQUIRE(kDefaultVisionLanguages[0] == "zh-Hans");
  REQUIRE(kDefaultVisionLanguages[1] == "en-US");

  AppleVisionOptions opts_a;
  AppleVisionOptions opts_b;
  REQUIRE(opts_a == opts_b);
  REQUIRE(opts_a.recognition_languages == std::vector<std::string>{"zh-Hans", "en-US"});

  // Stage registry singleton is accessible without Apple frameworks
  auto& reg = stage_registry();
  (void)reg;

#if defined(SUBLIFT_ENABLE_VISION) && SUBLIFT_ENABLE_VISION
  const auto caps = AppleVisionOcrEngine::probe_capabilities();
  REQUIRE(caps.available == is_vision_available());

  AppleVisionOcrEngine engine;
  REQUIRE(engine.engine_name() == "apple_vision");
  REQUIRE(engine.recognition_languages() == std::vector<std::string>{"zh-Hans", "en-US"});
  REQUIRE(engine.runtime_stats().recognize_calls == 0);
  REQUIRE(engine.runtime_stats().recognized_lines == 0);

  AppleVisionOcrEngine custom_engine({"en-US"});
  REQUIRE(custom_engine.engine_name() == "apple_vision");
  REQUIRE(custom_engine.recognition_languages() == std::vector<std::string>{"en-US"});

  // Empty ImageView returns immediately without invoking Vision
  const OcrResult empty_res = engine.recognize(ImageView{});
  REQUIRE(empty_res.lines.empty());
  REQUIRE(empty_res.text.empty());
#else
  REQUIRE_FALSE(is_vision_available());
  const auto caps = AppleVisionOcrEngine::probe_capabilities();
  REQUIRE_FALSE(caps.available);
  REQUIRE(caps.detail.find("SUBLIFT_ENABLE_VISION=OFF") != std::string::npos);

  try {
    AppleVisionOcrEngine engine;
    FAIL("Expected std::runtime_error when SUBLIFT_ENABLE_VISION=OFF");
  } catch (const std::runtime_error& ex) {
    REQUIRE(std::string(ex.what()).find("SUBLIFT_ENABLE_VISION=OFF") != std::string::npos);
  }
  try {
    AppleVisionOcrEngine engine(std::vector<std::string>{"en-US"});
    FAIL("Expected std::runtime_error when SUBLIFT_ENABLE_VISION=OFF");
  } catch (const std::runtime_error& ex) {
    REQUIRE(std::string(ex.what()).find("SUBLIFT_ENABLE_VISION=OFF") != std::string::npos);
  }
#endif
}
