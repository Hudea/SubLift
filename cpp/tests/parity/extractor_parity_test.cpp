#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <vector>

#include "../src/ffmpeg/process_utils.hpp"
#include "sublift/bottom_crop_detector.hpp"
#include "sublift/ffmpeg.hpp"
#include "sublift/fixed_detector.hpp"
#include "sublift/roi_passthrough_detector.hpp"
#include "sublift/test_support.hpp"

using namespace sublift;
using namespace sublift::ffmpeg;
using namespace sublift::test_support;

namespace {

[[nodiscard]] TransformPolicy parse_transform_policy(const std::string& s) {
  if (s == "error") {
    return TransformPolicy::Error;
  }
  return TransformPolicy::FallbackFull;
}

/// Map C++ detector dynamic type to golden expected_detector_type
/// (Python: ClassName.lower().replace("detector", "")).
[[nodiscard]] std::string detector_type_key(const IDetector& det) {
  if (dynamic_cast<const BottomCropDetector*>(&det) != nullptr) {
    return "bottomcrop";
  }
  if (dynamic_cast<const FixedRegionDetector*>(&det) != nullptr) {
    return "fixedregion";
  }
  if (dynamic_cast<const RoiPassthroughDetector*>(&det) != nullptr) {
    return "roipassthrough";
  }
  return "unknown";
}

[[nodiscard]] FrameIOPlan run_plan_scenario(const ExtractorPlanFrameIoGolden& sc) {
  const TransformPolicy policy = parse_transform_policy(sc.on_unvalidated_transform);
  if (sc.source.has_value()) {
    return plan_frame_io_pure(sc.region_box, sc.mode, sc.source, policy);
  }
  // no_region / mode=full paths never probe
  const std::filesystem::path fake_path{"/tmp/fake.mp4"};
  return plan_frame_io(fake_path, sc.region_box, sc.mode, policy);
}

}  // namespace

TEST_CASE("extractor pure contracts parity vs frozen golden", "[parity][extractor][pure]") {
  const std::filesystem::path golden_path{SUBLIFT_PARITY_GOLDEN_EXTRACTOR};
  INFO("Loading golden from: " << golden_path.string());
  const auto golden = load_extractor_golden(golden_path);

  SECTION("validate_output_crop parity") {
    for (const auto& sc : golden.pure_validate_crop) {
      INFO("Scenario: " << sc.name);
      if (sc.expected_ok) {
        REQUIRE(sc.crop.has_value());
        REQUIRE_NOTHROW(validate_output_crop(*sc.crop, sc.source_width, sc.source_height));
      } else {
        try {
          REQUIRE(sc.crop.has_value());
          validate_output_crop(*sc.crop, sc.source_width, sc.source_height);
          FAIL("Expected std::invalid_argument for scenario: " << sc.name);
        } catch (const std::invalid_argument& exc) {
          std::string msg = exc.what();
          REQUIRE(sc.expected_error_contains.has_value());
          REQUIRE(msg.find(*sc.expected_error_contains) != std::string::npos);
        }
      }
    }
  }

  SECTION("build_output_vf parity") {
    for (const auto& sc : golden.pure_build_vf) {
      INFO("Scenario: " << sc.name);
      std::string actual_vf = build_output_vf(sc.fps, sc.crop);
      REQUIRE(actual_vf == sc.expected_vf);
    }
  }

  SECTION("assess_display_transform parity") {
    for (const auto& sc : golden.pure_assess_display) {
      INFO("Scenario: " << sc.name);
      auto [actual_ok, actual_note] = assess_display_transform(sc.stream_json);
      REQUIRE(actual_ok == sc.expected_ok);
      REQUIRE(actual_note == sc.expected_note);
    }
  }

  SECTION("plan_frame_io pure parity") {
    for (const auto& sc : golden.pure_plan_frame_io) {
      INFO("Scenario: " << sc.name);
      if (!sc.expected_ok) {
        try {
          static_cast<void>(run_plan_scenario(sc));
          FAIL("Expected exception for scenario: " << sc.name);
        } catch (const std::exception& exc) {
          std::string msg = exc.what();
          REQUIRE(sc.expected_error_contains.has_value());
          REQUIRE(msg.find(*sc.expected_error_contains) != std::string::npos);
        }
        continue;
      }

      auto plan = run_plan_scenario(sc);
      REQUIRE(plan.output_crop == sc.expected_output_crop);
      std::string mode_str =
          (plan.output_mode == FrameOutputMode::FullRgb) ? "full_rgb" : "roi_rgb";
      REQUIRE(mode_str == sc.expected_output_mode);
      REQUIRE(plan.fallback_reason == sc.expected_fallback_reason);
      REQUIRE(plan.detector != nullptr);
      REQUIRE(detector_type_key(*plan.detector) == sc.expected_detector_type);

      // ROI success: detector must be RoiPassthrough with frame-local [0,0,w,h]
      if (plan.output_mode == FrameOutputMode::RoiRgb) {
        REQUIRE(sc.expected_detector_type == "roipassthrough");
        REQUIRE(plan.output_crop.has_value());
        auto* passthrough = dynamic_cast<RoiPassthroughDetector*>(plan.detector.get());
        REQUIRE(passthrough != nullptr);
        REQUIRE(passthrough->box() ==
                FrameLocalBox{0, 0, plan.output_crop->width, plan.output_crop->height});
      }
    }
  }
}

TEST_CASE("extractor extraction parity vs frozen golden", "[parity][extractor][extract]") {
  if (!available()) {
    WARN("ffmpeg/ffprobe not available; skipping extractor extraction parity test");
    return;
  }

  const std::filesystem::path golden_path{SUBLIFT_PARITY_GOLDEN_EXTRACTOR};
  const auto golden = load_extractor_golden(golden_path);

  const std::filesystem::path tmp_video =
      std::filesystem::temp_directory_path() / "sublift_test_extractor_parity.mp4";

  std::error_code ec;
  std::filesystem::remove(tmp_video, ec);

  // Generate synthetic test video (320x240 3s 1fps)
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
      "3",
      "-pix_fmt",
      "yuv420p",
      "-y",
      tmp_video.string(),
  };

  auto gen_res = detail::run_subprocess(gen_cmd, std::chrono::milliseconds(15000));
  REQUIRE(gen_res.exit_code == 0);
  REQUIRE(std::filesystem::exists(tmp_video));

  for (const auto& sc : golden.extract_scenarios) {
    INFO("Extract scenario: " << sc.name);
    FfmpegExtractor extractor(sc.fps, sc.crop);
    if (sc.cancel_before_extract) {
      extractor.cancel();
    }

    if (!sc.expected_ok) {
      try {
        extractor.extract(tmp_video, [&](Frame) { return true; });
        FAIL("Expected extract failure for scenario: " << sc.name);
      } catch (const std::exception& exc) {
        std::string msg = exc.what();
        REQUIRE(sc.expected_error_contains.has_value());
        REQUIRE(msg.find(*sc.expected_error_contains) != std::string::npos);
      }
      continue;
    }

    std::vector<Frame> frames;
    extractor.extract(tmp_video, [&](Frame frame) {
      frames.push_back(std::move(frame));
      return true;
    });

    REQUIRE(static_cast<int>(frames.size()) == sc.expected_frame_count);

    std::vector<std::int64_t> actual_ts;
    for (const auto& f : frames) {
      actual_ts.push_back(f.timestamp_ms);
    }
    REQUIRE(actual_ts == sc.expected_timestamps_ms);

    if (!frames.empty()) {
      REQUIRE(frames[0].image.width() == sc.expected_width);
      REQUIRE(frames[0].image.height() == sc.expected_height);

      for (const auto& sc_f : sc.frames) {
        REQUIRE(sc_f.frame_index >= 0);
        REQUIRE(sc_f.frame_index < static_cast<int>(frames.size()));

        const auto& cand_img = frames[sc_f.frame_index].image;
        const uint8_t* ptr = cand_img.data();
        const std::int32_t stride = cand_img.stride_bytes();

        for (const auto& px : sc_f.sampled_pixels) {
          const std::size_t idx = px.y * stride + px.x * 3;
          int r = ptr[idx];
          int g = ptr[idx + 1];
          int b = ptr[idx + 2];
          REQUIRE(r == px.rgb[0]);
          REQUIRE(g == px.rgb[1]);
          REQUIRE(b == px.rgb[2]);
        }
      }
    }
  }

  std::filesystem::remove(tmp_video, ec);
}
