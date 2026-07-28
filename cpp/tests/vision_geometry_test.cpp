#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <vector>

#include "sublift/vision.hpp"

using namespace sublift;

TEST_CASE("bankers_round unit tests", "[vision][geometry][bankers_round]") {
  SECTION("round half to even cases matching Python 3 round()") {
    REQUIRE(bankers_round(0.5) == 0);
    REQUIRE(bankers_round(1.5) == 2);
    REQUIRE(bankers_round(2.5) == 2);
    REQUIRE(bankers_round(3.5) == 4);
    REQUIRE(bankers_round(-0.5) == 0);
    REQUIRE(bankers_round(-1.5) == -2);
    REQUIRE(bankers_round(-2.5) == -2);
  }

  SECTION("regular rounding non-half cases") {
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
}

TEST_CASE("vision availability and constructor tests", "[vision][availability]") {
#if defined(SUBLIFT_ENABLE_VISION) && SUBLIFT_ENABLE_VISION
  SECTION("is_vision_available returns true on macOS when VISION=ON") {
    REQUIRE(is_vision_available());
  }

  SECTION("VisionOcrEngine constructs successfully when VISION=ON") {
    REQUIRE_NOTHROW(VisionOcrEngine());
    VisionOcrEngine engine({"en-US"});
    REQUIRE(engine.recognition_languages().size() == 1);
    REQUIRE(engine.recognition_languages()[0] == "en-US");
  }
#else
  SECTION("is_vision_available stub returns false when VISION=OFF") {
    REQUIRE_FALSE(is_vision_available());
  }

  SECTION("VisionOcrEngine constructor throws std::runtime_error when VISION=OFF") {
    try {
      VisionOcrEngine engine;
      FAIL("Expected std::runtime_error when SUBLIFT_ENABLE_VISION=OFF");
    } catch (const std::runtime_error& exc) {
      std::string msg = exc.what();
      REQUIRE(msg.find("SUBLIFT_ENABLE_VISION=OFF") != std::string::npos);
    }
  }
#endif

  SECTION("kDefaultVisionLanguages defaults to zh-Hans and en-US") {
    REQUIRE(kDefaultVisionLanguages.size() == 2);
    REQUIRE(kDefaultVisionLanguages[0] == "zh-Hans");
    REQUIRE(kDefaultVisionLanguages[1] == "en-US");
  }
}

TEST_CASE("clamp_ocr_box pure geometry tests", "[vision][geometry][clamp]") {
  SECTION("zero or negative image bounds return zero box") {
    REQUIRE(clamp_ocr_box(10, 20, 100, 50, 0, 1080) == OcrCropBox{0, 0, 0, 0});
    REQUIRE(clamp_ocr_box(10, 20, 100, 50, 1920, 0) == OcrCropBox{0, 0, 0, 0});
    REQUIRE(clamp_ocr_box(10, 20, 100, 50, -100, -100) == OcrCropBox{0, 0, 0, 0});
  }

  SECTION("normal in-bounds box remains unchanged") {
    REQUIRE(clamp_ocr_box(10, 20, 100, 50, 1920, 1080) == OcrCropBox{10, 20, 100, 50});
  }

  SECTION("negative origin clamped to zero") {
    REQUIRE(clamp_ocr_box(-10, -5, 100, 50, 1920, 1080) == OcrCropBox{0, 0, 100, 50});
  }

  SECTION("out of right and bottom bounds truncated") {
    REQUIRE(clamp_ocr_box(1900, 1050, 100, 100, 1920, 1080) == OcrCropBox{1900, 1050, 20, 30});
  }

  SECTION("origin beyond image bounds truncated to image edge with zero size") {
    REQUIRE(clamp_ocr_box(2000, 500, 50, 50, 1920, 1080) == OcrCropBox{1920, 500, 0, 50});
    REQUIRE(clamp_ocr_box(500, 1200, 50, 50, 1920, 1080) == OcrCropBox{500, 1080, 50, 0});
  }

  SECTION("full edge touch box") {
    REQUIRE(clamp_ocr_box(0, 0, 1920, 1080, 1920, 1080) == OcrCropBox{0, 0, 1920, 1080});
  }
}

TEST_CASE("vision_normalized_box_to_pixel conversion tests", "[vision][geometry][box_to_pixel]") {
  SECTION("full image normalized box") {
    REQUIRE(vision_normalized_box_to_pixel(0.0, 0.0, 1.0, 1.0, 1920, 1080) ==
            OcrCropBox{0, 0, 1920, 1080});
  }

  SECTION("top-left quadrant box (Vision ny=0.5, nh=0.5)") {
    REQUIRE(vision_normalized_box_to_pixel(0.0, 0.5, 0.5, 0.5, 1920, 1080) ==
            OcrCropBox{0, 0, 960, 540});
  }

  SECTION("bottom-right quadrant box (Vision ny=0.0, nh=0.5)") {
    REQUIRE(vision_normalized_box_to_pixel(0.5, 0.0, 0.5, 0.5, 1920, 1080) ==
            OcrCropBox{960, 540, 960, 540});
  }

  SECTION("exact rounding accuracy") {
    REQUIRE(vision_normalized_box_to_pixel(0.1, 0.2, 0.3, 0.4, 1000, 1000) ==
            OcrCropBox{100, 400, 300, 400});
  }

  SECTION("out-of-bounds normalized box automatically clamped") {
    REQUIRE(vision_normalized_box_to_pixel(-0.1, 1.2, 0.5, 0.5, 1000, 1000) ==
            OcrCropBox{0, 0, 500, 500});
  }

  SECTION("bankers rounding .5 cases in normalized box to pixel") {
    // 0.0005 * 1000 = 0.5 -> 0 (even)
    // (1 - 0.0 - 0.0025) * 1000 = 997.5 -> 998 (even)
    // 0.0015 * 1000 = 1.5 -> 2 (even)
    // 0.0025 * 1000 = 2.5 -> 2 (even)
    REQUIRE(vision_normalized_box_to_pixel(0.0005, 0.0, 0.0015, 0.0025, 1000, 1000) ==
            OcrCropBox{0, 998, 2, 2});
  }

  SECTION("zero or negative image size returns zero box") {
    REQUIRE(vision_normalized_box_to_pixel(0.1, 0.1, 0.5, 0.5, 0, 1000) == OcrCropBox{0, 0, 0, 0});
    REQUIRE(vision_normalized_box_to_pixel(0.1, 0.1, 0.5, 0.5, 1000, -1) == OcrCropBox{0, 0, 0, 0});
  }
}

TEST_CASE("sort_ocr_lines ordering tests", "[vision][geometry][sort]") {
  SECTION("empty and single element vectors") {
    std::vector<OcrLine> empty_lines;
    sort_ocr_lines(empty_lines);
    REQUIRE(empty_lines.empty());

    std::vector<OcrLine> single_line = {OcrLine{"hello", 0.9, OcrCropBox{10, 20, 30, 40}}};
    sort_ocr_lines(single_line);
    REQUIRE(single_line.size() == 1);
    REQUIRE(single_line[0].text == "hello");
  }

  SECTION("multiple lines sorted by y ascending then x ascending") {
    std::vector<OcrLine> lines = {
        OcrLine{"line1", 0.8, OcrCropBox{100, 50, 40, 10}},
        OcrLine{"line2", 0.9, OcrCropBox{20, 10, 40, 10}},
        OcrLine{"line3", 0.95, OcrCropBox{10, 50, 40, 10}},
    };

    sort_ocr_lines(lines);

    REQUIRE(lines[0].text == "line2");  // y=10, x=20
    REQUIRE(lines[1].text == "line3");  // y=50, x=10
    REQUIRE(lines[2].text == "line1");  // y=50, x=100
  }

  SECTION("stable sort preserves order for identical coordinates") {
    std::vector<OcrLine> lines = {
        OcrLine{"first", 0.8, OcrCropBox{30, 20, 40, 10}},
        OcrLine{"second", 0.9, OcrCropBox{30, 20, 40, 10}},
    };

    sort_ocr_lines(lines);

    REQUIRE(lines[0].text == "first");
    REQUIRE(lines[1].text == "second");
  }
}

// Offline collect-path negatives (mirrors Python TestCollectResults; no Vision.framework).
TEST_CASE("collect-path pure: blank strip + empty from_lines", "[vision][geometry][collect]") {
  SECTION("is_ocr_text_blank matches Python str.strip emptiness") {
    REQUIRE(is_ocr_text_blank(""));
    REQUIRE(is_ocr_text_blank("  \t\n\r"));
    REQUIRE(is_ocr_text_blank("\xc2\xa0"));          // U+00A0 NBSP
    REQUIRE(is_ocr_text_blank("\xe3\x80\x80"));      // U+3000 ideographic space
    REQUIRE(is_ocr_text_blank(" \xe3\x80\x80 \t"));
    REQUIRE_FALSE(is_ocr_text_blank("a"));
    REQUIRE_FALSE(is_ocr_text_blank("  hi  "));
    REQUIRE_FALSE(is_ocr_text_blank("\xe3\x80\x80x"));
  }

  SECTION("whitespace-only lines dropped before from_lines → empty structure") {
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
    const OcrResult result = OcrResult::from_lines(kept);
    REQUIRE(result.text.empty());
    REQUIRE(result.confidence == 0.0);
    REQUIRE(result.lines.empty());
  }

  SECTION("empty observations → from_lines empty structure (L0)") {
    const OcrResult result = OcrResult::from_lines({});
    REQUIRE(result.text.empty());
    REQUIRE(result.confidence == 0.0);
    REQUIRE(result.lines.empty());
  }

  SECTION("sort then from_lines joins like Python _collect_results") {
    std::vector<OcrLine> lines = {
        OcrLine{"bottom", 0.5, OcrCropBox{0, 70, 100, 30}},
        OcrLine{"top", 0.9, OcrCropBox{0, 0, 100, 30}},
    };
    sort_ocr_lines(lines);
    const OcrResult result = OcrResult::from_lines(lines);
    REQUIRE(result.text == "top\nbottom");
    REQUIRE(result.lines.size() == 2);
    REQUIRE(result.lines[0].text == "top");
    REQUIRE(result.lines[1].text == "bottom");
    REQUIRE(result.confidence == Catch::Approx(0.7));
  }
}
