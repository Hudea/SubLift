#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <filesystem>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <type_traits>

#include <sublift/core/box.hpp>
#include <sublift/core/frame.hpp>
#include <sublift/core/geometry.hpp>
#include <sublift/core/image_buffer.hpp>
#include <sublift/core/models.hpp>
#include <sublift/core/time.hpp>
#include <sublift/core/version.hpp>
#include <sublift/ports/detector.hpp>
#include <sublift/ports/frame_source.hpp>
#include <sublift/ports/ocr_engine.hpp>
#include <sublift/ports/subtitle_sink.hpp>

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
  REQUIRE(src.right() == 100);
  REQUIRE(src.bottom() == 30);
  REQUIRE(ocr.height == 4);
  REQUIRE_FALSE(src.empty());
  REQUIRE_FALSE(fl.empty());
  REQUIRE_FALSE(ocr.empty());
  REQUIRE_FALSE(region.empty());

  // Compile-time verification: strong boxes cannot be implicitly converted
  STATIC_REQUIRE_FALSE(std::is_convertible_v<SourceBox, FrameLocalBox>);
  STATIC_REQUIRE_FALSE(std::is_convertible_v<FrameLocalBox, OcrCropBox>);
  STATIC_REQUIRE_FALSE(std::is_convertible_v<OcrCropBox, SourceBox>);
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
  SubtitleEntry entry{.id = 0, .range = {0, 1000}, .text = "hi"};
  REQUIRE(entry.confidence == 1.0);
  REQUIRE(entry.start_ms() == 0);
  REQUIRE(entry.end_ms() == 1000);
  REQUIRE(entry.duration_ms() == 1000);
}

TEST_CASE("Box2i geometric operations", "[models]") {
  Box2i b{10, 20, 100, 50};
  REQUIRE(b.right() == 110);
  REQUIRE(b.bottom() == 70);

  // Point containment: half-open [x, right), [y, bottom)
  REQUIRE(b.contains(10, 20));
  REQUIRE(b.contains(109, 69));
  REQUIRE_FALSE(b.contains(110, 20)); // Right edge excluded
  REQUIRE_FALSE(b.contains(10, 70));  // Bottom edge excluded
  REQUIRE_FALSE(b.contains(9, 20));
  REQUIRE_FALSE(b.contains(10, 19));

  // Box containment
  Box2i inside{20, 30, 50, 30};
  REQUIRE(b.contains(inside));
  Box2i crossing{20, 30, 100, 30};
  REQUIRE_FALSE(b.contains(crossing));

  // Box intersection & predicate
  Box2i overlapping{50, 30, 100, 50};
  REQUIRE(b.intersects(overlapping));
  Box2i inter = b.intersection(overlapping);
  REQUIRE(inter.x == 50);
  REQUIRE(inter.y == 30);
  REQUIRE(inter.width == 60);
  REQUIRE(inter.height == 40);

  // Disjoint & touching boxes
  Box2i touching{110, 20, 50, 50};
  REQUIRE_FALSE(b.intersects(touching));
  REQUIRE(b.intersection(touching).empty());

  // Empty box properties
  Box2i empty_box{0, 0, 0, 0};
  REQUIRE(empty_box.empty());
  REQUIRE_FALSE(empty_box.contains(0, 0));
  REQUIRE_FALSE(b.contains(empty_box));
}

TEST_CASE("TimeRange geometric operations and DEV-002", "[models]") {
  // DEV-002: 0ms is a valid start timestamp
  TimeRange valid_zero{0, 200};
  REQUIRE_FALSE(valid_zero.empty());
  REQUIRE(valid_zero.start_ms == 0);
  REQUIRE(valid_zero.end_ms == 200);
  REQUIRE(valid_zero.duration_ms() == 200);
  REQUIRE(valid_zero.contains(0));
  REQUIRE(valid_zero.contains(199));
  REQUIRE_FALSE(valid_zero.contains(200));

  // Sub-range containment
  TimeRange sub{50, 150};
  REQUIRE(valid_zero.contains(sub));

  // Overlap
  TimeRange overlap{100, 300};
  REQUIRE(valid_zero.overlaps(overlap));
  REQUIRE(overlap.overlaps(valid_zero));

  // Touching boundary does not overlap
  TimeRange adjacent{200, 400};
  REQUIRE_FALSE(valid_zero.overlaps(adjacent));
  REQUIRE_FALSE(adjacent.overlaps(valid_zero));

  // Empty / inverted ranges
  TimeRange zero_len{100, 100};
  REQUIRE(zero_len.empty());
  TimeRange inverted{200, 100};
  REQUIRE(inverted.empty());
  REQUIRE_FALSE(valid_zero.contains(inverted));
  REQUIRE_FALSE(valid_zero.overlaps(inverted));
}

TEST_CASE("SubtitleProfile::from_character_band and extreme coordinates", "[models]") {
  constexpr auto i32_max = std::numeric_limits<std::int32_t>::max();

  // 1. Regular inputs
  const auto prof_normal = SubtitleProfile::from_character_band(1920, 800, 900, 40);
  REQUIRE(prof_normal.script == "auto");
  REQUIRE(prof_normal.center_x == 960);
  REQUIRE(prof_normal.center_y == 850);
  REQUIRE(prof_normal.height == 40);
  REQUIRE(prof_normal.y_min == 800);
  REQUIRE(prof_normal.y_max == 900);

  // 2. Inverted / negative band clamping
  const auto prof_inv = SubtitleProfile::from_character_band(-100, 900, 800, -10);
  REQUIRE(prof_inv.center_x == 0);
  REQUIRE(prof_inv.y_min == 900);
  REQUIRE(prof_inv.y_max == 900); // max(y0, y_max) = max(900, 800) = 900
  REQUIRE(prof_inv.center_y == 900);
  REQUIRE(prof_inv.height == 1); // max(1, 900 - 900) = 1

  // 3. Regression test: coordinates near INT32_MAX / 2 (prevents signed integer wrap)
  // y_min = 1073741824, y_max = 1073741824 -> y0 + y1 = 2147483648 > INT32_MAX
  const std::int32_t big_y = 1073741824;
  const auto prof_mid_overflow = SubtitleProfile::from_character_band(100, big_y, big_y, 20);
  REQUIRE(prof_mid_overflow.center_x == 50);
  REQUIRE(prof_mid_overflow.center_y == big_y);
  REQUIRE(prof_mid_overflow.height == 20);
  REQUIRE(prof_mid_overflow.y_min == big_y);
  REQUIRE(prof_mid_overflow.y_max == big_y);

  // 4. Coordinates near INT32_MAX
  const auto prof_max = SubtitleProfile::from_character_band(i32_max, i32_max - 200, i32_max, 0);
  REQUIRE(prof_max.center_x == i32_max / 2);
  REQUIRE(prof_max.center_y == i32_max - 100);
  REQUIRE(prof_max.height == 200);
  REQUIRE(prof_max.y_min == i32_max - 200);
  REQUIRE(prof_max.y_max == i32_max);
}

TEST_CASE("Box2i extreme coordinates and half-open boundaries", "[models]") {
  constexpr auto i32_max = std::numeric_limits<std::int32_t>::max();
  constexpr auto i32_min = std::numeric_limits<std::int32_t>::min();

  // 1. Extreme coordinates (x + width > INT32_MAX)
  Box2i big_box{1500000000, 1500000000, 1000000000, 1000000000};
  REQUIRE(big_box.right() == 2500000000LL);
  REQUIRE(big_box.bottom() == 2500000000LL);
  REQUIRE(big_box.contains(1500000000, 1500000000));
  REQUIRE(big_box.contains(1500000005, 1500000005));
  REQUIRE(big_box.contains(i32_max, i32_max));
  REQUIRE_FALSE(big_box.contains(1499999999, 1500000000));

  // 2. Half-open interval 8-boundary points check on Box2i{10, 20, 30, 40} -> [10, 40) x [20, 60)
  Box2i b{10, 20, 30, 40};
  REQUIRE(b.right() == 40);
  REQUIRE(b.bottom() == 60);
  REQUIRE_FALSE(b.contains(9, 20));   // x - 1
  REQUIRE(b.contains(10, 20));        // x (inclusive top-left)
  REQUIRE(b.contains(39, 20));        // right - 1
  REQUIRE_FALSE(b.contains(40, 20));  // right (exclusive)
  REQUIRE_FALSE(b.contains(10, 19));  // y - 1
  REQUIRE(b.contains(10, 59));        // bottom - 1
  REQUIRE_FALSE(b.contains(10, 60));  // bottom (exclusive)
  REQUIRE_FALSE(b.contains(40, 60));  // bottom-right corner (exclusive)
  REQUIRE(b.contains(39, 59));        // interior bottom-right

  // 3. 1x1 unit box boundaries
  Box2i unit{42, 84, 1, 1};
  REQUIRE_FALSE(unit.empty());
  REQUIRE(unit.right() == 43);
  REQUIRE(unit.bottom() == 85);
  REQUIRE(unit.contains(42, 84));
  REQUIRE_FALSE(unit.contains(41, 84));
  REQUIRE_FALSE(unit.contains(43, 84));
  REQUIRE_FALSE(unit.contains(42, 83));
  REQUIRE_FALSE(unit.contains(42, 85));

  // 4. Coordinates near INT32_MAX
  Box2i near_max{i32_max - 100, i32_max - 100, 100, 100};
  REQUIRE_FALSE(near_max.empty());
  REQUIRE(near_max.right() == i32_max);
  REQUIRE(near_max.bottom() == i32_max);
  REQUIRE(near_max.contains(i32_max - 100, i32_max - 100));
  REQUIRE(near_max.contains(i32_max - 1, i32_max - 1));
  REQUIRE_FALSE(near_max.contains(i32_max, i32_max - 1)); // right excluded
  REQUIRE_FALSE(near_max.contains(i32_max - 1, i32_max)); // bottom excluded

  // 5. Coordinates near INT32_MIN
  Box2i near_min{i32_min, i32_min, 100, 100};
  REQUIRE_FALSE(near_min.empty());
  REQUIRE(near_min.right() == static_cast<std::int64_t>(i32_min) + 100);
  REQUIRE(near_min.bottom() == static_cast<std::int64_t>(i32_min) + 100);
  REQUIRE(near_min.contains(i32_min, i32_min));
  REQUIRE(near_min.contains(i32_min + 99, i32_min + 99));
  REQUIRE_FALSE(near_min.contains(i32_min + 100, i32_min + 99));

  // 6. Degenerate and negative dimensions
  Box2i min_dim{0, 0, i32_min, i32_min};
  REQUIRE(min_dim.empty());
  REQUIRE_FALSE(min_dim.contains(0, 0));

  Box2i neg_dim{10, 10, -5, 20};
  REQUIRE(neg_dim.empty());
  REQUIRE_FALSE(neg_dim.contains(10, 10));

  // 7. Extreme intersection
  Box2i b_outer{i32_max - 200, i32_max - 200, 200, 200};
  Box2i b_inner{i32_max - 100, i32_max - 100, 100, 100};
  REQUIRE(b_outer.contains(b_inner));
  REQUIRE(b_outer.intersects(b_inner));
  REQUIRE(b_outer.intersection(b_inner) == b_inner);

  // 8. Generic intersect_boxes with extreme coordinates
  SourceBox s1{1500000000, 1500000000, 1000000000, 1000000000};
  SourceBox s2{1600000000, 1600000000, 100, 100};
  SourceBox s_inter = intersect_boxes(s1, s2);
  REQUIRE(s_inter.x == 1600000000);
  REQUIRE(s_inter.y == 1600000000);
  REQUIRE(s_inter.width == 100);
  REQUIRE(s_inter.height == 100);
}

namespace {

class PortsTestSource : public sublift::IFrameSource {
 public:
  sublift::VideoInfo probe(const std::filesystem::path&) override {
    return sublift::VideoInfo{.width = 1280, .height = 720, .duration_ms = 5000, .fps = 30.0};
  }
  sublift::FrameIOPlan plan(const sublift::VideoInfo&,
                            const std::optional<sublift::SourceBox>& crop) override {
    return sublift::FrameIOPlan{.output_crop = crop, .output_mode = sublift::FrameOutputMode::FullRgb};
  }
  void extract(const std::filesystem::path&,
               double,
               const std::optional<sublift::SourceBox>&,
               std::function<bool(sublift::Frame)>) override {}
  sublift::ImageBuffer extract_frame(const std::filesystem::path&,
                                     std::int64_t,
                                     const std::optional<sublift::SourceBox>&) override {
    return sublift::ImageBuffer{};
  }
  void cancel() override { cancelled_ = true; }
  bool is_cancelled() const noexcept override { return cancelled_; }

 private:
  bool cancelled_{false};
};

class PortsTestOcr : public sublift::IOcrEngine {
 public:
  sublift::OcrResult recognize(const sublift::ImageView&) override {
    return sublift::OcrResult{.text = "Recognized", .confidence = 0.99, .lines = {}};
  }
  std::string engine_name() const override { return "TestOcr"; }
};

class PortsTestSink : public sublift::ISubtitleSink {
 public:
  void emit(const sublift::SubtitleEntry& entry) override { last_entry_ = entry; }
  void flush() override { flushed_ = true; }
  void close() override { closed_ = true; }
  sublift::SubtitleEntry last_entry_{};
  bool flushed_{false};
  bool closed_{false};
};

class PortsTestDetector : public sublift::IDetector {
 public:
  std::optional<sublift::Region> detect(const sublift::Frame&) override {
    return sublift::Region{sublift::FrameLocalBox{0, 600, 1280, 120}};
  }
};

}  // namespace

TEST_CASE("Core version macro and string", "[core][models]") {
  REQUIRE_FALSE(sublift::version_string().empty());
  REQUIRE(SUBLIFT_VERSION_MAJOR == 1);
  REQUIRE(SUBLIFT_VERSION_MINOR == 0);
  REQUIRE(SUBLIFT_VERSION_PATCH == 0);
  REQUIRE(sublift::version_string() == "1.0.0");
}

TEST_CASE("Ports polymorphic contract dispatch", "[ports]") {
  SECTION("IFrameSource interface polymorphism") {
    std::unique_ptr<sublift::IFrameSource> src = std::make_unique<PortsTestSource>();
    auto info = src->probe("dummy.mp4");
    REQUIRE(info.width == 1280);
    REQUIRE(info.height == 720);
    REQUIRE(info.fps == 30.0);

    auto plan = src->plan(info, std::nullopt);
    REQUIRE(plan.output_mode == sublift::FrameOutputMode::FullRgb);

    src->cancel();
    REQUIRE(src->is_cancelled());
  }

  SECTION("IOcrEngine and OcrResult from_lines") {
    std::unique_ptr<sublift::IOcrEngine> ocr = std::make_unique<PortsTestOcr>();
    REQUIRE(ocr->engine_name() == "TestOcr");

    sublift::ImageView empty_view;
    auto res = ocr->recognize(empty_view);
    REQUIRE(res.text == "Recognized");
    REQUIRE(res.confidence > 0.9);

    std::vector<sublift::OcrLine> lines = {
        {.text = "Line1", .confidence = 0.8, .box = sublift::OcrCropBox{0, 0, 10, 10}},
        {.text = "Line2", .confidence = 0.9, .box = sublift::OcrCropBox{0, 10, 10, 10}},
    };
    auto from_lines_res = sublift::OcrResult::from_lines(lines);
    REQUIRE(from_lines_res.text == "Line1\nLine2");
    REQUIRE(std::abs(from_lines_res.confidence - 0.85) < 1e-5);
  }

  SECTION("ISubtitleSink contract") {
    PortsTestSink sink;
    sublift::ISubtitleSink* p_sink = &sink;
    p_sink->emit(sublift::SubtitleEntry{
        .id = 42,
        .range = sublift::TimeRange{100, 500},
        .text = "Sample",
        .confidence = 0.95,
    });
    p_sink->flush();
    p_sink->close();

    REQUIRE(sink.last_entry_.id == 42);
    REQUIRE(sink.flushed_);
    REQUIRE(sink.closed_);
  }

  SECTION("IDetector contract") {
    std::unique_ptr<sublift::IDetector> detector = std::make_unique<PortsTestDetector>();
    sublift::Frame frame{.timestamp_ms = 0, .image = sublift::ImageBuffer{}};
    auto region = detector->detect(frame);
    REQUIRE(region.has_value());
    REQUIRE(region->box.height == 120);
  }
}
