#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

#include <sublift/adapters/ffmpeg/ffmpeg_frame_source.hpp>
#include <sublift/ports/frame_source.hpp>
#include "display_transform_internal.hpp"

using namespace sublift;
using namespace sublift::adapters::ffmpeg;

TEST_CASE("validate_output_crop validation rules", "[extractor][pure]") {
  SECTION("Valid crops") {
    REQUIRE_NOTHROW(validate_output_crop(SourceBox{0, 0, 1920, 1080}, 1920, 1080));
    REQUIRE_NOTHROW(validate_output_crop(SourceBox{100, 200, 300, 400}, 1920, 1080));
    REQUIRE_NOTHROW(validate_output_crop(SourceBox{100, 100, 1820, 980}, 1920, 1080));
    REQUIRE_NOTHROW(validate_output_crop(SourceBox{0, 0, 319, 239}, 640, 480));
  }

  SECTION("Negative x/y coordinates") {
    try {
      validate_output_crop(SourceBox{-1, 0, 100, 100}, 1920, 1080);
      FAIL("Should have thrown std::invalid_argument");
    } catch (const std::invalid_argument& exc) {
      const std::string msg = exc.what();
      REQUIRE(msg.find("x/y 不能为负") != std::string::npos);
      REQUIRE(msg.find("requested=[-1, 0, 100, 100]") != std::string::npos);
      REQUIRE(msg.find("source=1920x1080") != std::string::npos);
    }

    try {
      validate_output_crop(SourceBox{0, -10, 100, 100}, 1920, 1080);
      FAIL("Should have thrown std::invalid_argument");
    } catch (const std::invalid_argument& exc) {
      const std::string msg = exc.what();
      REQUIRE(msg.find("x/y 不能为负") != std::string::npos);
    }
  }

  SECTION("Non-positive width/height") {
    try {
      validate_output_crop(SourceBox{0, 0, 0, 100}, 1920, 1080);
      FAIL("Should have thrown std::invalid_argument");
    } catch (const std::invalid_argument& exc) {
      const std::string msg = exc.what();
      REQUIRE(msg.find("width/height 必须为正") != std::string::npos);
    }

    try {
      validate_output_crop(SourceBox{0, 0, 100, -5}, 1920, 1080);
      FAIL("Should have thrown std::invalid_argument");
    } catch (const std::invalid_argument& exc) {
      const std::string msg = exc.what();
      REQUIRE(msg.find("width/height 必须为正") != std::string::npos);
    }
  }

  SECTION("Out of bounds crop") {
    try {
      validate_output_crop(SourceBox{1000, 0, 1000, 1080}, 1920, 1080);
      FAIL("Should have thrown std::invalid_argument");
    } catch (const std::invalid_argument& exc) {
      const std::string msg = exc.what();
      REQUIRE(msg.find("越界") != std::string::npos);
    }

    try {
      validate_output_crop(SourceBox{0, 500, 1920, 600}, 1920, 1080);
      FAIL("Should have thrown std::invalid_argument");
    } catch (const std::invalid_argument& exc) {
      const std::string msg = exc.what();
      REQUIRE(msg.find("越界") != std::string::npos);
    }
  }

  SECTION("Large int overflow protection") {
    try {
      validate_output_crop(SourceBox{2'000'000'000, 0, 1'000'000'000, 1080}, 1920, 1080);
      FAIL("Should have thrown std::invalid_argument due to out of bounds");
    } catch (const std::invalid_argument& exc) {
      const std::string msg = exc.what();
      REQUIRE(msg.find("越界") != std::string::npos);
    }
  }
}

TEST_CASE("build_output_vf string formatting", "[extractor][pure]") {
  SECTION("Full frame") {
    REQUIRE(build_output_vf(1.0, nullptr) == "fps=1.0");
    REQUIRE(build_output_vf(2.5, nullptr) == "fps=2.5");
    std::optional<SourceBox> no_crop = std::nullopt;
    REQUIRE(build_output_vf(1.0, no_crop) == "fps=1.0");
  }

  SECTION("ROI crop") {
    SourceBox crop{100, 50, 300, 200};
    REQUIRE(build_output_vf(1.0, &crop) ==
            "fps=1.0,format=rgb24,crop=300:200:100:50:exact=1");

    std::optional<SourceBox> opt_crop = SourceBox{0, 0, 1920, 1080};
    REQUIRE(build_output_vf(0.5, opt_crop) ==
            "fps=0.5,format=rgb24,crop=1920:1080:0:0:exact=1");
  }
}

TEST_CASE("plan_two_stage_crop and build_optimized_output_vf", "[extractor][pure]") {
  SECTION("plan_two_stage_crop geometry invariants") {
    auto check_invariants = [](const TwoStageCropPlan& plan,
                               const SourceBox& original,
                               std::int32_t sw,
                               std::int32_t sh) {
      if (sh % 2 == 0) {
        REQUIRE(plan.coarse_crop.y % 2 == 0);
        REQUIRE(plan.coarse_crop.height % 2 == 0);
      }
      if (sw >= 32 && sw % 32 == 0) {
        REQUIRE(plan.coarse_crop.x % 32 == 0);
        REQUIRE(plan.coarse_crop.width % 32 == 0);
      } else if (sw >= 16 && sw % 16 == 0) {
        REQUIRE(plan.coarse_crop.x % 16 == 0);
        REQUIRE(plan.coarse_crop.width % 16 == 0);
      } else {
        REQUIRE(plan.coarse_crop.x == 0);
        REQUIRE(plan.coarse_crop.width == sw);
      }

      // Coarse crop must fit inside source frame
      REQUIRE(plan.coarse_crop.x >= 0);
      REQUIRE(plan.coarse_crop.y >= 0);
      REQUIRE(plan.coarse_crop.x + plan.coarse_crop.width <= sw);
      REQUIRE(plan.coarse_crop.y + plan.coarse_crop.height <= sh);

      // Exact crop offsets must fit inside coarse crop
      REQUIRE(plan.exact_crop.x >= 0);
      REQUIRE(plan.exact_crop.y >= 0);
      REQUIRE(plan.exact_crop.x + plan.exact_crop.width <= plan.coarse_crop.width);
      REQUIRE(plan.exact_crop.y + plan.exact_crop.height <= plan.coarse_crop.height);

      // Exact crop dimensions match requested
      REQUIRE(plan.exact_crop.width == original.width);
      REQUIRE(plan.exact_crop.height == original.height);

      // Absolute coordinates match
      REQUIRE(plan.coarse_crop.x + plan.exact_crop.x == original.x);
      REQUIRE(plan.coarse_crop.y + plan.exact_crop.y == original.y);
    };

    // 1. Standard 1080p subtitle ROI (even y, odd h, default margin = 8)
    SourceBox crop1{0, 778, 1920, 121};
    auto plan1 = plan_two_stage_crop(crop1, 1920, 1080);
    check_invariants(plan1, crop1, 1920, 1080);
    REQUIRE(plan1.coarse_crop.x == 0);
    REQUIRE(plan1.coarse_crop.y == 770);
    REQUIRE(plan1.coarse_crop.width == 1920);
    REQUIRE(plan1.coarse_crop.height == 138);
    REQUIRE(plan1.exact_crop.x == 0);
    REQUIRE(plan1.exact_crop.y == 8);
    REQUIRE(plan1.exact_crop.width == 1920);
    REQUIRE(plan1.exact_crop.height == 121);

    // 2. Odd y and odd h (10-bit counterexample geometry: 0,779,1920,121)
    SourceBox crop2{0, 779, 1920, 121};
    auto plan2 = plan_two_stage_crop(crop2, 1920, 1080);
    check_invariants(plan2, crop2, 1920, 1080);
    REQUIRE(plan2.coarse_crop.y == 770);
    REQUIRE(plan2.coarse_crop.height == 138);
    REQUIRE(plan2.exact_crop.y == 9);

    // 3. Non-full-width odd box (333,777,1001,123 -> 32-aligned width & margin)
    SourceBox crop_nonfull{333, 777, 1001, 123};
    auto plan_nonfull = plan_two_stage_crop(crop_nonfull, 1920, 1080);
    check_invariants(plan_nonfull, crop_nonfull, 1920, 1080);
    REQUIRE(plan_nonfull.coarse_crop.x == 320);
    REQUIRE(plan_nonfull.coarse_crop.width == 1024);
    REQUIRE(plan_nonfull.exact_crop.x == 13);

    // 4. Non-full-width 16-aligned x/w aligns coarse box to 32 on 1920x1080 (AVX2+NEON safe)
    SourceBox crop_aligned16{320, 776, 1008, 120};
    auto plan_aligned16 = plan_two_stage_crop(crop_aligned16, 1920, 1080);
    check_invariants(plan_aligned16, crop_aligned16, 1920, 1080);
    REQUIRE(plan_aligned16.coarse_crop.x == 288);
    REQUIRE(plan_aligned16.coarse_crop.width == 1056);
    REQUIRE(plan_aligned16.exact_crop.x == 32);

    // 4b. 16-multiple but non-32-multiple source width (720x1280) aligns to 16
    SourceBox crop_720w{100, 960, 500, 90};
    auto plan_720w = plan_two_stage_crop(crop_720w, 720, 1280);
    check_invariants(plan_720w, crop_720w, 720, 1280);
    REQUIRE(plan_720w.coarse_crop.x == 80);
    REQUIRE(plan_720w.coarse_crop.width == 528);
    REQUIRE(plan_720w.exact_crop.x == 20);

    // 5. Top edge (y = 0) and left/right edge clamping
    SourceBox crop3{0, 0, 1920, 200};
    auto plan3 = plan_two_stage_crop(crop3, 1920, 1080);
    check_invariants(plan3, crop3, 1920, 1080);
    REQUIRE(plan3.coarse_crop.y == 0);
    REQUIRE(plan3.coarse_crop.height == 208);
    REQUIRE(plan3.exact_crop.y == 0);

    SourceBox crop_right{1905, 880, 15, 200};
    auto plan_right = plan_two_stage_crop(crop_right, 1920, 1080);
    check_invariants(plan_right, crop_right, 1920, 1080);
    REQUIRE(plan_right.coarse_crop.x == 1888);
    REQUIRE(plan_right.coarse_crop.width == 32);
    REQUIRE(plan_right.coarse_crop.y == 872);
    REQUIRE(plan_right.coarse_crop.height == 208);

    // 6. Non-16-multiple source width (854x480) preserves full source width
    SourceBox crop_480p{111, 360, 501, 54};
    auto plan_480p = plan_two_stage_crop(crop_480p, 854, 480);
    check_invariants(plan_480p, crop_480p, 854, 480);
    REQUIRE(plan_480p.coarse_crop.x == 0);
    REQUIRE(plan_480p.coarse_crop.width == 854);
    REQUIRE(plan_480p.coarse_crop.y == 352);
    REQUIRE(plan_480p.coarse_crop.height == 70);
    REQUIRE(plan_480p.exact_crop.x == 111);
    REQUIRE(plan_480p.exact_crop.y == 8);

    // 7. Margin 0, negative margin, and INT32_MAX margin overflow protection
    SourceBox crop7{0, 778, 1920, 120};
    auto plan7 = plan_two_stage_crop(crop7, 1920, 1080, 0);
    check_invariants(plan7, crop7, 1920, 1080);
    REQUIRE(plan7.coarse_crop.y == 778);
    REQUIRE(plan7.coarse_crop.height == 120);
    REQUIRE(plan7.exact_crop.y == 0);

    auto plan8 = plan_two_stage_crop(crop7, 1920, 1080, -4);
    check_invariants(plan8, crop7, 1920, 1080);
    REQUIRE(plan8.coarse_crop.y == 778);
    REQUIRE(plan8.coarse_crop.height == 120);

    auto plan_max_margin = plan_two_stage_crop(
        crop7, 1920, 1080, std::numeric_limits<std::int32_t>::max());
    check_invariants(plan_max_margin, crop7, 1920, 1080);
    REQUIRE(plan_max_margin.coarse_crop == SourceBox{0, 0, 1920, 1080});

    auto plan_min_margin = plan_two_stage_crop(
        crop7, 1920, 1080, std::numeric_limits<std::int32_t>::min());
    check_invariants(plan_min_margin, crop7, 1920, 1080);
    REQUIRE(plan_min_margin.coarse_crop.y == 778);
    REQUIRE(plan_min_margin.coarse_crop.height == 120);
  }

  SECTION("plan_two_stage_crop validation") {
    SourceBox invalid_crop{0, 0, 2000, 1080};
    REQUIRE_THROWS_AS(plan_two_stage_crop(invalid_crop, 1920, 1080), std::invalid_argument);
  }

  SECTION("build_optimized_output_vf string formatting and fallbacks") {
    std::optional<SourceBox> no_crop = std::nullopt;
    REQUIRE(build_optimized_output_vf(2.5, no_crop, 1920, 1080) == "fps=2.5");

    std::optional<SourceBox> crop = SourceBox{100, 50, 300, 200};
    REQUIRE(build_optimized_output_vf(1.0, crop, 0, 0) ==
            "fps=1.0,format=rgb24,crop=300:200:100:50:exact=1");
    REQUIRE(build_optimized_output_vf(1.0, crop, -1, 1080) ==
            "fps=1.0,format=rgb24,crop=300:200:100:50:exact=1");
    REQUIRE(build_optimized_output_vf(1.0, crop, 639, 480) ==
            "fps=1.0,format=rgb24,crop=300:200:100:50:exact=1");
    REQUIRE(build_optimized_output_vf(1.0, crop, 640, 479) ==
            "fps=1.0,format=rgb24,crop=300:200:100:50:exact=1");
    REQUIRE_THROWS_AS(
        build_optimized_output_vf(1.0, SourceBox{500, 50, 200, 200}, 639, 480),
        std::invalid_argument);

    std::optional<SourceBox> full_crop = SourceBox{0, 0, 1920, 1080};
    REQUIRE(build_optimized_output_vf(1.0, full_crop, 1920, 1080) ==
            "fps=1.0,format=rgb24,crop=1920:1080:0:0:exact=1");

    std::optional<SourceBox> opt_gt_crop = SourceBox{0, 778, 1920, 121};
    REQUIRE(build_optimized_output_vf(5.0, opt_gt_crop, 1920, 1080) ==
            "fps=5.0,crop=1920:138:0:770:exact=1,format=rgb24,crop=1920:121:0:8:exact=1");
  }

  SECTION("FfmpegExtractorOptions and crop_margin accessor") {
    FfmpegExtractorOptions default_opts;
    REQUIRE(default_opts.enable_two_stage_crop);
    REQUIRE(default_opts.crop_margin == kDefaultTwoStageCropMargin);
    REQUIRE(default_opts.crop_margin == 8);

    FfmpegFrameSource extractor(
        1.0, std::nullopt, std::nullopt, std::nullopt,
        FfmpegExtractorOptions{.enable_two_stage_crop = false, .crop_margin = 12});
    REQUIRE_FALSE(extractor.options().enable_two_stage_crop);
    REQUIRE(extractor.crop_margin() == 12);
  }
}

TEST_CASE("assess_display_transform and is_identity_display_matrix", "[extractor][pure]") {
  SECTION("No tags or side_data") {
    nlohmann::json stream = nlohmann::json::object({{"width", 1920}, {"height", 1080}});
    auto [ok, note] = detail::assess_display_transform(stream);
    REQUIRE(ok);
    REQUIRE_FALSE(note.has_value());
  }

  SECTION("tags.rotate == '0'") {
    nlohmann::json stream = nlohmann::json::object({
        {"width", 1920},
        {"height", 1080},
        {"tags", {{"rotate", "0"}}}
    });
    auto [ok, note] = detail::assess_display_transform(stream);
    REQUIRE(ok);
    REQUIRE_FALSE(note.has_value());
  }

  SECTION("tags.rotate == 90 (integer)") {
    nlohmann::json stream = nlohmann::json::object({
        {"width", 1920},
        {"height", 1080},
        {"tags", {{"rotate", 90}}}
    });
    auto [ok, note] = detail::assess_display_transform(stream);
    REQUIRE_FALSE(ok);
    REQUIRE(note.has_value());
    REQUIRE(note.value() == "stream.tags.rotate=90");
  }

  SECTION("tags.rotate == '90'") {
    nlohmann::json stream = nlohmann::json::object({
        {"width", 1920},
        {"height", 1080},
        {"tags", {{"rotate", "90"}}}
    });
    auto [ok, note] = detail::assess_display_transform(stream);
    REQUIRE_FALSE(ok);
    REQUIRE(note.has_value());
    REQUIRE(note.value() == "stream.tags.rotate='90'");
  }

  SECTION("side_data rotation == 0.0") {
    nlohmann::json stream = nlohmann::json::object({
        {"side_data_list", nlohmann::json::array({
            {{"side_data_type", "Display Matrix"}, {"rotation", 0.0}}
        })}
    });
    auto [ok, note] = detail::assess_display_transform(stream);
    REQUIRE(ok);
    REQUIRE_FALSE(note.has_value());
  }

  SECTION("side_data rotation == -90.0") {
    nlohmann::json stream = nlohmann::json::object({
        {"side_data_list", nlohmann::json::array({
            {{"side_data_type", "Display Matrix"}, {"rotation", -90.0}}
        })}
    });
    auto [ok, note] = detail::assess_display_transform(stream);
    REQUIRE_FALSE(ok);
    REQUIRE(note.has_value());
    REQUIRE(note.value().find("non-identity display matrix") != std::string::npos);
  }

  SECTION("16.16 fixed-point identity matrix") {
    nlohmann::json stream = nlohmann::json::object({
        {"side_data_list", nlohmann::json::array({
            {{"side_data_type", "Display Matrix"},
             {"displaymatrix", "65536 0 0 0 65536 0 0 0 1073741824"}}
        })}
    });
    auto [ok, note] = detail::assess_display_transform(stream);
    REQUIRE(ok);
    REQUIRE_FALSE(note.has_value());
  }

  SECTION("displaymatrix is null but matrix is identity array fallback") {
    nlohmann::json stream = nlohmann::json::object({
        {"side_data_list", nlohmann::json::array({
            {{"side_data_type", "Display Matrix"},
             {"displaymatrix", nullptr},
             {"matrix", nlohmann::json::array({1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0})}}
        })}
    });
    auto [ok, note] = detail::assess_display_transform(stream);
    REQUIRE(ok);
    REQUIRE_FALSE(note.has_value());
  }

  SECTION("3x3 float identity matrix array") {
    nlohmann::json stream = nlohmann::json::object({
        {"side_data_list", nlohmann::json::array({
            {{"side_data_type", "Display Matrix"},
             {"matrix", nlohmann::json::array({1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0})}}
        })}
    });
    auto [ok, note] = detail::assess_display_transform(stream);
    REQUIRE(ok);
    REQUIRE_FALSE(note.has_value());
  }

  SECTION("Non-identity matrix string") {
    nlohmann::json stream = nlohmann::json::object({
        {"side_data_list", nlohmann::json::array({
            {{"side_data_type", "Display Matrix"},
             {"displaymatrix", "0 -65536 0 65536 0 0 0 0 1073741824"}}
        })}
    });
    auto [ok, note] = detail::assess_display_transform(stream);
    REQUIRE_FALSE(ok);
    REQUIRE(note.has_value());
    REQUIRE(note.value().find("non-identity display matrix") != std::string::npos);
  }

  SECTION("Invalid side_data_list item") {
    nlohmann::json stream = nlohmann::json::object({
        {"side_data_list", nlohmann::json::array({"invalid_string"})}
    });
    auto [ok, note] = detail::assess_display_transform(stream);
    REQUIRE_FALSE(ok);
    REQUIRE(note.has_value());
    REQUIRE(note.value() == "stream.side_data_list 项不可解析");
  }
}

class DummyFrameSource : public IFrameSource {
 public:
  VideoInfo probe(const std::filesystem::path&) override { return {}; }
  FrameIOPlan plan(const VideoInfo&, const std::optional<SourceBox>&) override { return {}; }
  void extract(const std::filesystem::path&, double, const std::optional<SourceBox>&,
               std::function<bool(Frame)>) override {}
  ImageBuffer extract_frame(const std::filesystem::path&, std::int64_t,
                            const std::optional<SourceBox>&) override {
    return ImageBuffer(1, 1, PixelFormat::RGB24);
  }
  void cancel() override { cancelled_ = true; }
  bool is_cancelled() const noexcept override { return cancelled_; }

  bool cancelled_{false};
};

TEST_CASE("IFrameSource polymorphic usage", "[extractor][pure]") {
  DummyFrameSource dummy;
  IFrameSource& source = dummy;

  REQUIRE_FALSE(source.is_cancelled());
  source.cancel();
  REQUIRE(dummy.cancelled_);
  REQUIRE(source.is_cancelled());
}

TEST_CASE("Smart hwaccel resolution and 10-bit nv12 / max_output_width vf planning",
          "[extractor][pure]") {
  SECTION("is_high_bit_depth_pix_fmt classification") {
    REQUIRE(is_high_bit_depth_pix_fmt("yuv420p10le"));
    REQUIRE(is_high_bit_depth_pix_fmt("p010le"));
    REQUIRE(is_high_bit_depth_pix_fmt("yuv444p12le"));
    REQUIRE_FALSE(is_high_bit_depth_pix_fmt("yuv420p"));
    REQUIRE_FALSE(is_high_bit_depth_pix_fmt("nv12"));
    REQUIRE_FALSE(is_high_bit_depth_pix_fmt(""));
  }

  SECTION("resolve_effective_hwaccel dispatches auto cleanly and preserves overrides") {
    const StreamDecodeHints uhd_hevc10{
        .width = 3840,
        .height = 2160,
        .fps = 24.0,
        .codec_name = "hevc",
        .pix_fmt = "yuv420p10le",
    };
    const StreamDecodeHints hd_h264_8bit{
        .width = 1920,
        .height = 1080,
        .fps = 24.0,
        .codec_name = "h264",
        .pix_fmt = "yuv420p",
    };
    const StreamDecodeHints hd_h264_60fps{
        .width = 1920,
        .height = 1080,
        .fps = 60.0,
        .codec_name = "h264",
        .pix_fmt = "yuv420p",
    };

    REQUIRE(resolve_effective_hwaccel("none", uhd_hevc10).empty());
    REQUIRE(resolve_effective_hwaccel("off", uhd_hevc10).empty());
    REQUIRE(resolve_effective_hwaccel("0", uhd_hevc10).empty());
    REQUIRE(resolve_effective_hwaccel("cuda", hd_h264_8bit) == "cuda");
    REQUIRE(resolve_effective_hwaccel("auto", hd_h264_8bit).empty());

    const StreamDecodeHints uhd_h264_8bit{
        .width = 3840,
        .height = 2160,
        .fps = 24.0,
        .codec_name = "h264",
        .pix_fmt = "yuv420p",
    };
    const StreamDecodeHints hd_hevc_10bit{
        .width = 1920,
        .height = 1080,
        .fps = 24.0,
        .codec_name = "hevc",
        .pix_fmt = "yuv420p10le",
    };
#if defined(__APPLE__)
    REQUIRE(resolve_effective_hwaccel("auto", uhd_h264_8bit) == "videotoolbox");
    REQUIRE(resolve_effective_hwaccel("auto", hd_hevc_10bit) == "videotoolbox");
    REQUIRE(resolve_effective_hwaccel("auto", hd_h264_60fps) == "videotoolbox");
#else
    REQUIRE(resolve_effective_hwaccel("auto", uhd_h264_8bit).empty());
    REQUIRE(resolve_effective_hwaccel("auto", hd_hevc_10bit).empty());
    REQUIRE(resolve_effective_hwaccel("auto", hd_h264_60fps).empty());
#endif
  }

  SECTION("plan_scaled_output_vf handles 1080p passthrough vs 4K 10-bit VT scaling + nv12") {
    const FfmpegExtractorOptions opts_1920{
        .enable_two_stage_crop = true,
        .crop_margin = 8,
        .max_output_width = 1920,
    };

    const auto hd_plan = plan_scaled_output_vf(
        5.0, SourceBox{0, 840, 1920, 160}, 1920, 1080, opts_1920, "", "yuv420p");
    REQUIRE_FALSE(hd_plan.scaled);
    REQUIRE_FALSE(hd_plan.used_nv12_bridge);
    REQUIRE(hd_plan.out_width == 1920);
    REQUIRE(hd_plan.out_height == 160);
    REQUIRE(hd_plan.vf.find("scale=") == std::string::npos);

    const auto uhd_plan = plan_scaled_output_vf(
        5.0, SourceBox{0, 1760, 3840, 300}, 3840, 2160, opts_1920, "videotoolbox", "yuv420p10le");
    REQUIRE(uhd_plan.scaled);
    REQUIRE(uhd_plan.used_nv12_bridge);
    REQUIRE(uhd_plan.out_width == 1920);
    REQUIRE(uhd_plan.out_height == 150);
    REQUIRE(uhd_plan.out_height % 2 == 0);
    REQUIRE(uhd_plan.vf.find("format=nv12,format=rgb24") != std::string::npos);
    REQUIRE(uhd_plan.vf.find("scale=1920:150:flags=fast_bilinear") != std::string::npos);
  }

  SECTION("resolve_effective_skip_frame enforces >=49.5fps and 3x sample ratio in auto mode") {
    const StreamDecodeHints hd_24fps{.width = 1920, .height = 1080, .fps = 24.0, .codec_name = "h264", .pix_fmt = "yuv420p"};
    const StreamDecodeHints uhd_23976fps{.width = 3840, .height = 2160, .fps = 24000.0 / 1001.0, .codec_name = "hevc", .pix_fmt = "yuv420p10le"};
    const StreamDecodeHints hd_25fps{.width = 1920, .height = 1080, .fps = 25.0};
    const StreamDecodeHints hd_2997fps{.width = 1920, .height = 1080, .fps = 30000.0 / 1001.0};
    const StreamDecodeHints hd_30fps{.width = 1920, .height = 1080, .fps = 30.0};
    const StreamDecodeHints hd_48fps{.width = 1920, .height = 1080, .fps = 48.0};
    const StreamDecodeHints hd_4995fps{.width = 1920, .height = 1080, .fps = 49.95};
    const StreamDecodeHints hd_50fps{.width = 1920, .height = 1080, .fps = 50.0};
    const StreamDecodeHints hd_5994fps{.width = 1920, .height = 1080, .fps = 60000.0 / 1001.0};
    const StreamDecodeHints uhd_60fps{.width = 3840, .height = 2160, .fps = 60.0, .codec_name = "hevc", .pix_fmt = "yuv420p10le"};
    const StreamDecodeHints hd_120fps{.width = 1920, .height = 1080, .fps = 120.0};
    const StreamDecodeHints zero_fps{.width = 1920, .height = 1080, .fps = 0.0};

    // Sub-50fps streams stay empty in auto mode (both 1080p and 4K)
    REQUIRE(resolve_effective_skip_frame("auto", uhd_23976fps, 5.0).empty());
    REQUIRE(resolve_effective_skip_frame("", hd_24fps, 5.0).empty());
    REQUIRE(resolve_effective_skip_frame("auto", hd_25fps, 5.0).empty());
    REQUIRE(resolve_effective_skip_frame("auto", hd_2997fps, 5.0).empty());
    REQUIRE(resolve_effective_skip_frame("auto", hd_30fps, 5.0).empty());
    REQUIRE(resolve_effective_skip_frame("auto", hd_48fps, 5.0).empty());
    REQUIRE(resolve_effective_skip_frame("auto", zero_fps, 5.0).empty());

    // >=49.5fps streams with >=3x headroom auto-enable noref
    REQUIRE(resolve_effective_skip_frame("auto", hd_4995fps, 5.0) == "noref");
    REQUIRE(resolve_effective_skip_frame("auto", hd_50fps, 5.0) == "noref");
    REQUIRE(resolve_effective_skip_frame("auto", hd_5994fps, 5.0) == "noref");
    REQUIRE(resolve_effective_skip_frame("auto", uhd_60fps, 10.0) == "noref");
    REQUIRE(resolve_effective_skip_frame("auto", hd_120fps, 10.0) == "noref");

    // Dense sampling ratio guard (<3x) or non-positive sample_fps disables auto skipping
    REQUIRE(resolve_effective_skip_frame("auto", uhd_60fps, 25.0).empty());
    REQUIRE(resolve_effective_skip_frame("auto", uhd_60fps, 0.0).empty());

    // Explicit overrides
    REQUIRE(resolve_effective_skip_frame("noref", uhd_23976fps, 5.0) == "noref");
    REQUIRE(resolve_effective_skip_frame("NONREF", hd_24fps, 5.0) == "noref");
    REQUIRE(resolve_effective_skip_frame("none", uhd_60fps, 5.0).empty());
    REQUIRE(resolve_effective_skip_frame("off", uhd_60fps, 5.0).empty());
    REQUIRE(resolve_effective_skip_frame("false", uhd_60fps, 5.0).empty());
    REQUIRE(resolve_effective_skip_frame("0", uhd_60fps, 5.0).empty());
    REQUIRE_THROWS_AS(resolve_effective_skip_frame("bogus", uhd_60fps, 5.0), std::invalid_argument);
  }
}
