#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <memory>
#include <utility>

#include <sublift/adapters/ffmpeg/bottom_crop_detector.hpp>
#include <sublift/adapters/ffmpeg/ffmpeg_frame_source.hpp>
#include <sublift/adapters/ffmpeg/fixed_detector.hpp>
#include <sublift/adapters/ffmpeg/roi_passthrough_detector.hpp>
#include "../adapters/ffmpeg/src/process_utils.hpp"

using namespace sublift;
using namespace sublift::adapters::ffmpeg;

namespace {

[[nodiscard]] bool is_bottom_crop(const IDetector& d, double ratio = 0.3) {
  auto* p = dynamic_cast<const BottomCropDetector*>(&d);
  return p != nullptr && p->bottom_ratio() == ratio;
}

[[nodiscard]] bool is_fixed_region(const IDetector& d, const FrameLocalBox& box) {
  auto* p = dynamic_cast<const FixedRegionDetector*>(&d);
  return p != nullptr && p->region().box == box;
}

[[nodiscard]] bool is_roi_passthrough(const IDetector& d, std::int32_t w, std::int32_t h) {
  auto* p = dynamic_cast<const RoiPassthroughDetector*>(&d);
  return p != nullptr && p->box() == FrameLocalBox{0, 0, w, h};
}

}  // namespace

TEST_CASE("BottomCropDetector unit tests", "[extractor][roi][pure]") {
  SECTION("Default bottom ratio 0.3 on 1080p frame") {
    Frame frame_1080{0, ImageBuffer(1920, 1080, PixelFormat::RGB24)};
    BottomCropDetector detector(0.3);
    REQUIRE(detector.bottom_ratio() == 0.3);
    auto region = detector.detect(frame_1080);
    REQUIRE(region.has_value());
    REQUIRE(region->box.x == 0);
    REQUIRE(region->box.y == 756);
    REQUIRE(region->box.width == 1920);
    REQUIRE(region->box.height == 324);
  }

  SECTION("Ratio 0.5 on 320x240 frame") {
    Frame frame_240{0, ImageBuffer(320, 240, PixelFormat::RGB24)};
    BottomCropDetector detector(0.5);
    auto region = detector.detect(frame_240);
    REQUIRE(region.has_value());
    REQUIRE(region->box.x == 0);
    REQUIRE(region->box.y == 120);
    REQUIRE(region->box.width == 320);
    REQUIRE(region->box.height == 120);
  }

  SECTION("Ratio 0.0 on 320x240 frame") {
    Frame frame_240{0, ImageBuffer(320, 240, PixelFormat::RGB24)};
    BottomCropDetector detector(0.0);
    auto region = detector.detect(frame_240);
    REQUIRE(region.has_value());
    REQUIRE(region->box.x == 0);
    REQUIRE(region->box.y == 240);
    REQUIRE(region->box.width == 320);
    REQUIRE(region->box.height == 0);
  }

  SECTION("Ratio 1.0 on 320x240 frame") {
    Frame frame_240{0, ImageBuffer(320, 240, PixelFormat::RGB24)};
    BottomCropDetector detector(1.0);
    auto region = detector.detect(frame_240);
    REQUIRE(region.has_value());
    REQUIRE(region->box.x == 0);
    REQUIRE(region->box.y == 0);
    REQUIRE(region->box.width == 320);
    REQUIRE(region->box.height == 240);
  }
}

TEST_CASE("RoiPassthroughDetector unit tests", "[extractor][roi][pure]") {
  SECTION("Invalid non-positive dimensions throw std::invalid_argument") {
    REQUIRE_THROWS_AS(RoiPassthroughDetector(0, 100), std::invalid_argument);
    REQUIRE_THROWS_AS(RoiPassthroughDetector(100, -5), std::invalid_argument);
  }

  SECTION("Detect returns frame-local box [0, 0, w, h]") {
    RoiPassthroughDetector detector(320, 60);
    REQUIRE(detector.box() == FrameLocalBox{0, 0, 320, 60});

    Frame frame{0, ImageBuffer(320, 60, PixelFormat::RGB24)};
    auto region = detector.detect(frame);
    REQUIRE(region.has_value());
    REQUIRE(region->box == FrameLocalBox{0, 0, 320, 60});
  }
}

TEST_CASE("FrameIOPlan equality comparison", "[extractor][roi][pure]") {
  SECTION("Plans with equal fields are equal") {
    FrameIOPlan p1{
        .output_crop = SourceBox{0, 0, 100, 100},
        .output_mode = FrameOutputMode::RoiRgb,
        .source = std::nullopt,
        .fallback_reason = std::nullopt,
    };
    FrameIOPlan p2{
        .output_crop = SourceBox{0, 0, 100, 100},
        .output_mode = FrameOutputMode::RoiRgb,
        .source = std::nullopt,
        .fallback_reason = std::nullopt,
    };
    REQUIRE(p1 == p2);

    auto d1 = resolve_detector(p1, SourceBox{0, 0, 100, 100});
    auto d2 = resolve_detector(p2, SourceBox{0, 0, 100, 100});
    auto* r1 = dynamic_cast<const RoiPassthroughDetector*>(d1.get());
    auto* r2 = dynamic_cast<const RoiPassthroughDetector*>(d2.get());
    REQUIRE(r1 != nullptr);
    REQUIRE(r2 != nullptr);
    REQUIRE(r1->box() == r2->box());
  }

  SECTION("Plans with different crops or modes are not equal") {
    FrameIOPlan p1{
        .output_crop = std::nullopt,
        .output_mode = FrameOutputMode::FullRgb,
        .source = std::nullopt,
        .fallback_reason = std::nullopt,
    };
    FrameIOPlan p2{
        .output_crop = SourceBox{0, 0, 100, 100},
        .output_mode = FrameOutputMode::RoiRgb,
        .source = std::nullopt,
        .fallback_reason = std::nullopt,
    };
    REQUIRE_FALSE(p1 == p2);

    auto d1 = resolve_detector(p1, std::nullopt, 0.3);
    auto d2 = resolve_detector(p1, std::nullopt, 0.5);
    auto* b1 = dynamic_cast<const BottomCropDetector*>(d1.get());
    auto* b2 = dynamic_cast<const BottomCropDetector*>(d2.get());
    REQUIRE(b1 != nullptr);
    REQUIRE(b2 != nullptr);
    REQUIRE(b1->bottom_ratio() != b2->bottom_ratio());
  }
}

TEST_CASE("plan_frame_io mode and fallback rules", "[extractor][roi][pure]") {
  SECTION("Invalid mode throws std::invalid_argument") {
    std::filesystem::path fake_path{"/tmp/fake.mp4"};
    REQUIRE_THROWS_AS(plan_frame_io(fake_path, std::nullopt, "invalid_mode"),
                      std::invalid_argument);
  }

  SECTION("mode=roi requires region_box") {
    std::filesystem::path fake_path{"/tmp/fake.mp4"};
    REQUIRE_THROWS_AS(plan_frame_io(fake_path, std::nullopt, "roi"),
                      std::invalid_argument);
  }

  SECTION("No region_box without source returns FullRgb mode with BottomCropDetector") {
    auto plan = plan_frame_io_pure(std::nullopt, "auto", std::nullopt, TransformPolicy::FallbackFull, 0.3);
    REQUIRE_FALSE(plan.output_crop.has_value());
    REQUIRE(plan.output_mode == FrameOutputMode::FullRgb);
    REQUIRE_FALSE(plan.source.has_value());
    REQUIRE_FALSE(plan.fallback_reason.has_value());
    auto det = resolve_detector(plan, std::nullopt, 0.3);
    REQUIRE(det != nullptr);
    REQUIRE(is_bottom_crop(*det, 0.3));
  }

  SECTION("No region_box with valid source pushes bottom_ratio crop down to RoiRgb") {
    SourceFrameInfo ok_source{1920, 1080, true, std::nullopt, 1, 1, 0};
    auto plan = plan_frame_io_pure(std::nullopt, "auto", ok_source, TransformPolicy::FallbackFull, 0.3);
    REQUIRE(plan.output_crop.has_value());
    REQUIRE(*plan.output_crop == SourceBox{0, 756, 1920, 324});
    REQUIRE(plan.output_mode == FrameOutputMode::RoiRgb);
    REQUIRE(plan.source.has_value());
    REQUIRE_FALSE(plan.fallback_reason.has_value());
    auto det = resolve_detector(plan, std::nullopt, 0.3);
    REQUIRE(det != nullptr);
    REQUIRE(is_roi_passthrough(*det, 1920, 324));
  }

  SECTION("No region_box with unvalidated transform falls back to FullRgb") {
    SourceFrameInfo bad_source{1920, 1080, false, "rotate_90", 1, 1, 0};
    auto plan = plan_frame_io_pure(std::nullopt, "auto", bad_source, TransformPolicy::FallbackFull, 0.3);
    REQUIRE_FALSE(plan.output_crop.has_value());
    REQUIRE(plan.output_mode == FrameOutputMode::FullRgb);
    REQUIRE(plan.source.has_value());
    REQUIRE(plan.fallback_reason.has_value());
    REQUIRE(*plan.fallback_reason == "rotate_90");
    auto det = resolve_detector(plan, std::nullopt, 0.3);
    REQUIRE(det != nullptr);
    REQUIRE(is_bottom_crop(*det, 0.3));
  }

  SECTION("mode=full returns FullRgb mode with FixedRegionDetector") {
    std::filesystem::path fake_path{"/tmp/fake.mp4"};
    SourceBox region{100, 200, 300, 400};
    auto plan = plan_frame_io(fake_path, region, "full");
    REQUIRE_FALSE(plan.output_crop.has_value());
    REQUIRE(plan.output_mode == FrameOutputMode::FullRgb);
    REQUIRE_FALSE(plan.source.has_value());
    REQUIRE_FALSE(plan.fallback_reason.has_value());
    auto det = resolve_detector(plan, region);
    REQUIRE(det != nullptr);
    REQUIRE(is_fixed_region(*det, FrameLocalBox{100, 200, 300, 400}));
  }

  SECTION("mode=auto + display_transform_ok=false → FullRgb + FixedRegion + fallback_reason") {
    SourceBox region{0, 180, 320, 60};
    SourceFrameInfo bad_source{
        .width = 320,
        .height = 240,
        .display_transform_ok = false,
        .transform_note = "rotate=90",
    };
    auto plan = plan_frame_io_pure(region, "auto", bad_source, TransformPolicy::FallbackFull);
    REQUIRE_FALSE(plan.output_crop.has_value());
    REQUIRE(plan.output_mode == FrameOutputMode::FullRgb);
    REQUIRE(plan.source.has_value());
    REQUIRE(plan.source->display_transform_ok == false);
    REQUIRE(plan.fallback_reason.has_value());
    REQUIRE(*plan.fallback_reason == "rotate=90");
    auto det = resolve_detector(plan, region);
    REQUIRE(det != nullptr);
    REQUIRE(is_fixed_region(*det, FrameLocalBox{0, 180, 320, 60}));
  }

  SECTION("mode=auto + transform not ok + Error policy throws std::runtime_error") {
    SourceBox region{0, 180, 320, 60};
    SourceFrameInfo bad_source{
        .width = 320,
        .height = 240,
        .display_transform_ok = false,
        .transform_note = "rotate=90",
    };
    try {
      static_cast<void>(
          plan_frame_io_pure(region, "auto", bad_source, TransformPolicy::Error));
      FAIL("expected std::runtime_error");
    } catch (const std::runtime_error& exc) {
      std::string msg = exc.what();
      REQUIRE(msg.find("显示变换") != std::string::npos);
      REQUIRE(msg.find("rotate=90") != std::string::npos);
      REQUIRE(msg.find("320x240") != std::string::npos);
    }
  }

  SECTION("mode=roi + transform not ok throws std::runtime_error (even FallbackFull)") {
    SourceBox region{0, 180, 320, 60};
    SourceFrameInfo bad_source{
        .width = 320,
        .height = 240,
        .display_transform_ok = false,
        .transform_note = "rotate=90",
    };
    try {
      static_cast<void>(plan_frame_io_pure(region, "roi", bad_source,
                                           TransformPolicy::FallbackFull));
      FAIL("expected std::runtime_error");
    } catch (const std::runtime_error& exc) {
      std::string msg = exc.what();
      REQUIRE(msg.find("显示变换") != std::string::npos);
      REQUIRE(msg.find("note='rotate=90'") != std::string::npos);
    }
  }

  SECTION("mode=roi success with transform_ok → RoiRgb + RoiPassthrough + output_crop") {
    SourceBox region{0, 180, 320, 60};
    SourceFrameInfo ok_source{
        .width = 320,
        .height = 240,
        .display_transform_ok = true,
        .transform_note = std::nullopt,
    };
    auto plan = plan_frame_io_pure(region, "roi", ok_source, TransformPolicy::Error);
    REQUIRE(plan.output_crop.has_value());
    REQUIRE(*plan.output_crop == region);
    REQUIRE(plan.output_mode == FrameOutputMode::RoiRgb);
    REQUIRE(plan.source.has_value());
    REQUIRE_FALSE(plan.fallback_reason.has_value());
    auto det = resolve_detector(plan, region);
    REQUIRE(det != nullptr);
    REQUIRE(is_roi_passthrough(*det, 320, 60));
    auto* passthrough = dynamic_cast<RoiPassthroughDetector*>(det.get());
    REQUIRE(passthrough != nullptr);
    REQUIRE(passthrough->box() == FrameLocalBox{0, 0, 320, 60});
  }

  SECTION("mode=auto success with transform_ok → RoiRgb + RoiPassthrough") {
    SourceBox region{10, 10, 160, 120};
    SourceFrameInfo ok_source{
        .width = 320,
        .height = 240,
        .display_transform_ok = true,
    };
    auto plan = plan_frame_io_pure(region, "auto", ok_source);
    REQUIRE(plan.output_crop.has_value());
    REQUIRE(*plan.output_crop == region);
    REQUIRE(plan.output_mode == FrameOutputMode::RoiRgb);
    auto det = resolve_detector(plan, region);
    REQUIRE(det != nullptr);
    REQUIRE(is_roi_passthrough(*det, 160, 120));
  }

  SECTION("empty transform_note fallback uses unvalidated_display_transform") {
    SourceBox region{0, 180, 320, 60};
    SourceFrameInfo bad_source{
        .width = 320,
        .height = 240,
        .display_transform_ok = false,
        .transform_note = std::nullopt,
    };
    auto plan = plan_frame_io_pure(region, "auto", bad_source, TransformPolicy::FallbackFull);
    REQUIRE(plan.fallback_reason.has_value());
    REQUIRE(*plan.fallback_reason == "unvalidated_display_transform");
    auto det = resolve_detector(plan, region);
    REQUIRE(det != nullptr);
    REQUIRE(is_fixed_region(*det, FrameLocalBox{0, 180, 320, 60}));
  }
}

TEST_CASE("plan_frame_io integration test with synthetic video", "[extractor][roi][integration]") {
  if (!available()) {
    WARN("ffmpeg/ffprobe not available; skipping plan_frame_io integration test");
    return;
  }

  const std::filesystem::path tmp_video =
      std::filesystem::temp_directory_path() / "sublift_test_plan_roi.mp4";

  std::error_code ec;
  std::filesystem::remove(tmp_video, ec);

  // Generate 320x240 1s video
  const std::string ffmpeg_bin = resolve_ffmpeg_bin();
  const std::vector<std::string> gen_cmd = {
      ffmpeg_bin,
      "-nostdin",
      "-v",
      "error",
      "-f",
      "lavfi",
      "-i",
      "testsrc=size=320x240:rate=1",
      "-t",
      "1",
      "-pix_fmt",
      "yuv420p",
      "-y",
      tmp_video.string(),
  };

  auto gen_res = detail::run_subprocess(gen_cmd, std::chrono::milliseconds(15000));
  REQUIRE(gen_res.exit_code == 0);
  REQUIRE(std::filesystem::exists(tmp_video));

  SECTION("Valid region mode=auto returns RoiRgb and RoiPassthroughDetector") {
    SourceBox region{0, 180, 320, 60};
    auto plan = plan_frame_io(tmp_video, region, "auto");
    REQUIRE(plan.output_crop.has_value());
    REQUIRE(*plan.output_crop == region);
    REQUIRE(plan.output_mode == FrameOutputMode::RoiRgb);
    REQUIRE(plan.source.has_value());
    REQUIRE(plan.source->width == 320);
    REQUIRE(plan.source->height == 240);
    REQUIRE_FALSE(plan.fallback_reason.has_value());
    auto det = resolve_detector(plan, region);
    REQUIRE(det != nullptr);
    REQUIRE(is_roi_passthrough(*det, 320, 60));
  }

  SECTION("No region mode=auto on real video probes and pushes bottom_ratio crop down") {
    auto plan = plan_frame_io(tmp_video, std::nullopt, "auto", TransformPolicy::FallbackFull, 0.3);
    REQUIRE(plan.output_crop.has_value());
    // 240 * 0.3 = 72; 240 - 72 = 168
    REQUIRE(*plan.output_crop == SourceBox{0, 168, 320, 72});
    REQUIRE(plan.output_mode == FrameOutputMode::RoiRgb);
    REQUIRE(plan.source.has_value());
    REQUIRE(plan.source->width == 320);
    REQUIRE(plan.source->height == 240);
    auto det = resolve_detector(plan, std::nullopt, 0.3);
    REQUIRE(det != nullptr);
    REQUIRE(is_roi_passthrough(*det, 320, 72));
  }

  SECTION("Valid region mode=roi returns RoiRgb and RoiPassthroughDetector") {
    SourceBox region{0, 180, 320, 60};
    auto plan = plan_frame_io(tmp_video, region, "roi");
    REQUIRE(plan.output_crop.has_value());
    REQUIRE(*plan.output_crop == region);
    REQUIRE(plan.output_mode == FrameOutputMode::RoiRgb);
    auto det = resolve_detector(plan, region);
    REQUIRE(det != nullptr);
    REQUIRE(is_roi_passthrough(*det, 320, 60));
    auto* passthrough = dynamic_cast<RoiPassthroughDetector*>(det.get());
    REQUIRE(passthrough != nullptr);
    REQUIRE(passthrough->box() == FrameLocalBox{0, 0, 320, 60});
  }

  SECTION("Out-of-bounds region in plan_frame_io throws std::invalid_argument") {
    SourceBox invalid_region{0, 0, 1000, 1000};
    REQUIRE_THROWS_AS(plan_frame_io(tmp_video, invalid_region, "auto"),
                      std::invalid_argument);
  }

  std::filesystem::remove(tmp_video, ec);
}
