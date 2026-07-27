#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <limits>

#include "sublift/models.hpp"

using namespace sublift;

TEST_CASE("SCRIPT constants and validation", "[models]") {
  REQUIRE(SCRIPT_CJK == "cjk");
  REQUIRE(SCRIPT_LATIN == "latin");
  REQUIRE(SCRIPT_AUTO == "auto");
  REQUIRE(is_valid_script(SCRIPT_CJK));
  REQUIRE(is_valid_script(SCRIPT_LATIN));
  REQUIRE(is_valid_script(SCRIPT_AUTO));
  REQUIRE_FALSE(is_valid_script("foo"));
  REQUIRE_FALSE(is_valid_script(""));
}

TEST_CASE("strong boxes and Region", "[models]") {
  SourceBox src{.x = 0, .y = 10, .width = 100, .height = 20};
  FrameLocalBox fl{.x = 0, .y = 0, .width = 100, .height = 20};
  OcrCropBox ocr{.x = 1, .y = 2, .width = 3, .height = 4};
  Region region{.box = fl};
  REQUIRE(region.box.width == 100);
  REQUIRE(src.y == 10);
  REQUIRE(ocr.height == 4);
}

TEST_CASE("OcrResult::from_lines empty", "[models]") {
  const auto result = OcrResult::from_lines({});
  REQUIRE(result.text.empty());
  REQUIRE(result.confidence == 0.0);
  REQUIRE(result.lines.empty());
}

TEST_CASE("OcrResult::from_lines multi", "[models]") {
  const OcrLine lines[] = {
      {.text = "a", .confidence = 0.5, .box = {}},
      {.text = "b", .confidence = 1.0, .box = {}},
  };
  const auto result = OcrResult::from_lines(lines);
  REQUIRE(result.text == "a\nb");
  REQUIRE(result.confidence == Catch::Approx(0.75));
  REQUIRE(result.lines.size() == 2);
}

TEST_CASE("SubtitleProfile::from_crop", "[models]") {
  const auto profile = SubtitleProfile::from_crop(100, 40);
  REQUIRE(profile.script == "auto");
  REQUIRE(profile.center_x == 50);
  REQUIRE(profile.center_y == 20);
  REQUIRE(profile.height == 40);
  REQUIRE(profile.y_min == 0);
  REQUIRE(profile.y_max == 40);
}

TEST_CASE("SubtitleProfile::from_selection_in_video", "[models]") {
  SourceBox region{.x = 0, .y = 800, .width = 1920, .height = 100};
  SourceBox selection{.x = 400, .y = 820, .width = 200, .height = 40};
  const auto profile = SubtitleProfile::from_selection_in_video(region, selection);
  REQUIRE(profile.center_x == 500);
  REQUIRE(profile.center_y == 40);
  REQUIRE(profile.y_min == 20);
  REQUIRE(profile.y_max == 60);
  REQUIRE(profile.height == 40);
}

TEST_CASE("SubtitleProfile selection clamp", "[models]") {
  SourceBox region{.x = 0, .y = 0, .width = 100, .height = 50};
  SourceBox selection{.x = -10, .y = -5, .width = 200, .height = 100};
  const auto profile = SubtitleProfile::from_selection_in_video(region, selection);
  REQUIRE(profile.y_min == 0);
  REQUIRE(profile.y_max == 50);
  REQUIRE(profile.center_x == 50);
}

TEST_CASE("SubtitleProfile large coords without int32 wrap", "[models]") {
  // Python uses arbitrary-precision ints; C++ must not wrap rel+width in int32.
  SourceBox region{.x = 0, .y = 0, .width = 2147483647, .height = 1};
  SourceBox selection{.x = 1073741824, .y = 0, .width = 1073741824, .height = 1};
  const auto profile = SubtitleProfile::from_selection_in_video(region, selection);
  REQUIRE(profile.center_x == 1610612735);  // x0=1073741824, band_w=1073741823
  REQUIRE(profile.height == 1);
}

TEST_CASE("Frame timestamp_ms is int64", "[models]") {
  Frame frame;
  frame.timestamp_ms = static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max()) + 1;
  REQUIRE(frame.timestamp_ms > std::numeric_limits<std::int32_t>::max());
}

TEST_CASE("SubtitleEntry default confidence", "[models]") {
  SubtitleEntry entry{.start_ms = 0, .end_ms = 1000, .text = "hi"};
  REQUIRE(entry.confidence == 1.0);
}
