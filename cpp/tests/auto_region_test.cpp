#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>

#include <sublift/adapters/mock/mock_frame_source.hpp>
#include <sublift/adapters/mock/mock_ocr.hpp>
#include <sublift/engine/auto_region.hpp>

using namespace sublift;
using namespace sublift::adapters::mock;

namespace {

ImageBuffer create_synthetic_thumbnail_with_energy(int row_start, int row_end) {
  ImageBuffer buf = ImageBuffer::allocate_uninitialized(320, 80, PixelFormat::Gray8);
  std::memset(buf.data(), 0, 320 * 80);

  // Draw high-frequency vertical stripes in corridor [85, 235] with 4-pixel periods
  for (int y = row_start; y <= row_end && y < 80; ++y) {
    auto* row = buf.row(y);
    for (int x = 85; x <= 235; ++x) {
      row[x] = ((x / 4) % 2 == 0) ? 255 : 0;
    }
  }
  return buf;
}

ImageBuffer create_synthetic_bimodal_thumbnail(
    int up_r0, int up_r1, int lo_r0, int lo_r1, int x0, int x1) {
  ImageBuffer buf = ImageBuffer::allocate_uninitialized(320, 80, PixelFormat::Gray8);
  std::memset(buf.data(), 0, 320 * 80);

  const auto fill_band = [&](int r0, int r1) {
    for (int y = r0; y <= r1 && y < 80; ++y) {
      auto* row = buf.row(y);
      for (int x = x0; x <= x1 && x < 320; ++x) {
        row[x] = ((x / 4) % 2 == 0) ? 255 : 0;
      }
    }
  };
  fill_band(up_r0, up_r1);
  fill_band(lo_r0, lo_r1);
  return buf;
}

} // namespace

TEST_CASE("Auto-Region Two-Stage Funnel - Full Pipeline Success", "[engine][auto_region]") {
  MockFrameSource source;
  source.set_video_info(VideoInfo{
      .width = 1920,
      .height = 1080,
      .duration_ms = 120000,
      .fps = 25.0,
  });

  // Stage 1: Add keyframe thumbnails (ts=10000 has strong text energy in rows 30-42)
  std::vector<KeyframeThumbnail> thumbnails;
  thumbnails.push_back(KeyframeThumbnail{
      .timestamp_ms = 10000,
      .image = create_synthetic_thumbnail_with_energy(30, 42),
  });
  thumbnails.push_back(KeyframeThumbnail{
      .timestamp_ms = 25000,
      .image = create_synthetic_thumbnail_with_energy(30, 42),
  });
  thumbnails.push_back(KeyframeThumbnail{
      .timestamp_ms = 50000,
      .image = ImageBuffer(320, 80, PixelFormat::Gray8), // blank
  });
  source.set_keyframe_thumbnails(std::move(thumbnails));

  // Add full-res frames
  source.add_frame(Frame{
      .timestamp_ms = 10000,
      .image = ImageBuffer(1920, 1080, PixelFormat::RGB24),
  });
  source.add_frame(Frame{
      .timestamp_ms = 25000,
      .image = ImageBuffer(1920, 1080, PixelFormat::RGB24),
  });

  // Stage 2: Configure Mock OCR
  // Note: detect_auto_region extracts bottom 50% ROI (y_offset = 540)
  // Video text at y=880 maps to local OCR coordinate y = 880 - 540 = 340
  MockOcrEngine ocr;
  ocr.set_lines(std::vector<OcrLine>{
      OcrLine{
          .text = "动物城全自动字幕探测测试",
          .confidence = 0.96,
          .box = OcrCropBox{600, 340, 720, 40},
      },
  });

  AutoRegionOptions opts{
      .script = "cjk",
      .bottom_ratio = 0.45,
  };

  auto result = detect_auto_region(source, ocr, "test.mp4", opts);

  REQUIRE(result.detected);
  CHECK(source.sample_call_count() == 1);
  CHECK(result.sample_timestamp_ms == 10000);
  CHECK(result.preview_text == "动物城全自动字幕探测测试");
  CHECK(result.confidence >= 0.90);

  // Region box should envelope global y=880, h=40 with padding
  CHECK(result.region_box.x == 0);
  CHECK(result.region_box.width == 1920);
  CHECK(result.region_box.y <= 880);
  CHECK(result.region_box.y + result.region_box.height >= 920);

  // Profile must be in crop-local coordinates (relative to region_box)
  CHECK(result.profile.script == "cjk");
  CHECK(result.profile.y_min < result.region_box.height);
  CHECK(result.profile.y_max <= result.region_box.height);
  CHECK(result.profile.center_y > 0);
}

TEST_CASE("Auto-Region Smooth Fallback on Zero Detections", "[engine][auto_region]") {
  MockFrameSource source;
  source.set_video_info(VideoInfo{
      .width = 1920,
      .height = 1080,
      .duration_ms = 60000,
      .fps = 25.0,
  });

  MockOcrEngine ocr;
  ocr.set_lines({}); // No text detected

  AutoRegionOptions opts{
      .script = "cjk",
      .bottom_ratio = 0.35,
      .fallback_to_default = true,
  };

  auto result = detect_auto_region(source, ocr, "test.mp4", opts);

  CHECK_FALSE(result.detected);
  CHECK(result.region_box.x == 0);
  CHECK(result.region_box.width == 1920);
  CHECK(result.region_box.height == static_cast<int>(1080 * 0.35));
  CHECK(result.region_box.y == 1080 - result.region_box.height);
  CHECK(result.profile.script == "cjk");
  CHECK(result.error_message.find("平滑回退") != std::string::npos);
}

TEST_CASE("Auto-Region Uniform 4-Point Sampling Fallback when Thumbnails Empty", "[engine][auto_region]") {
  MockFrameSource source;
  source.set_video_info(VideoInfo{
      .width = 1920,
      .height = 1080,
      .duration_ms = 100000,
      .fps = 25.0,
  });
  // No thumbnails set

  // Add frames at uniform fallback points (dur=100000 -> 20000, 40000, 60000, 80000)
  source.add_frame(Frame{
      .timestamp_ms = 20000,
      .image = ImageBuffer(1920, 1080, PixelFormat::RGB24),
  });
  source.add_frame(Frame{
      .timestamp_ms = 40000,
      .image = ImageBuffer(1920, 1080, PixelFormat::RGB24),
  });

  MockOcrEngine ocr;
  ocr.set_lines(std::vector<OcrLine>{
      OcrLine{
          .text = "兜底时间等分抽样检测对白",
          .confidence = 0.94,
          .box = OcrCropBox{650, 350, 620, 42},
      },
  });

  AutoRegionOptions opts{
      .script = "cjk",
  };

  auto result = detect_auto_region(source, ocr, "test.mp4", opts);

  REQUIRE(result.detected);
  CHECK(result.region_box.width == 1920);
  CHECK(result.profile.script == "cjk");
  CHECK(result.preview_text == "兜底时间等分抽样检测对白");
}

TEST_CASE("Auto-Region Robustness - NaN/Invalid Bottom Ratio & Fallback Invariants", "[engine][auto_region][regression]") {
  MockFrameSource source;
  source.set_video_info(VideoInfo{
      .width = 1920,
      .height = 1080,
      .duration_ms = 10000,
      .fps = 25.0,
  });
  MockOcrEngine ocr;

  SECTION("NaN and negative bottom_ratio fallback safely") {
    AutoRegionOptions opts_nan{
        .bottom_ratio = std::numeric_limits<double>::quiet_NaN(),
        .fallback_to_default = true,
    };
    auto res_nan = detect_auto_region(source, ocr, "test.mp4", opts_nan);
    CHECK_FALSE(res_nan.detected);
    CHECK(res_nan.region_box.width == 1920);
    CHECK(res_nan.region_box.height == static_cast<int>(1080 * 0.45));
    CHECK(res_nan.region_box.y == 1080 - res_nan.region_box.height);

    AutoRegionOptions opts_neg{
        .bottom_ratio = -0.5,
        .fallback_to_default = true,
    };
    auto res_neg = detect_auto_region(source, ocr, "test.mp4", opts_neg);
    CHECK_FALSE(res_neg.detected);
    CHECK(res_neg.region_box.width == 1920);
    CHECK(res_neg.region_box.height == static_cast<int>(1080 * 0.45));
    CHECK(res_neg.region_box.y == 1080 - res_neg.region_box.height);
  }

  SECTION("Divergent or empty candidates trigger clean fallback without negative height") {
    source.add_frame(Frame{
        .timestamp_ms = 2000,
        .image = ImageBuffer(1920, 1080, PixelFormat::RGB24),
    });
    // Whitespace line produces no valid cluster
    ocr.set_lines(std::vector<OcrLine>{
        OcrLine{.text = "   ", .confidence = 0.9, .box = OcrCropBox{0, 0, 10, 10}},
    });
    AutoRegionOptions opts{
        .min_ocr_candidates = 1,
        .fallback_to_default = true,
    };
    auto res = detect_auto_region(source, ocr, "test.mp4", opts);
    CHECK_FALSE(res.detected);
    CHECK(res.region_box.height > 0);
    CHECK(res.region_box.y >= 0);
    CHECK(res.region_box.y + res.region_box.height <= 1080);
  }
}

TEST_CASE("Auto-Region Stage 2b Bimodal Recovery Expands Dual-Line Upper Bound", "[engine][auto_region][regression]") {
  MockFrameSource source;
  source.set_video_info(VideoInfo{
      .width = 1920,
      .height = 1080,
      .duration_ms = 120000,
      .fps = 25.0,
  });

  // Two high-energy 1-line keyframes rank #1/#2 in Stage 1 so Stage 2a stops on them;
  // a bimodal 2-line keyframe at 42000ms has lower 12-row energy so it awaits Stage 2b.
  std::vector<KeyframeThumbnail> thumbnails;
  thumbnails.push_back(KeyframeThumbnail{
      .timestamp_ms = 10000,
      .image = create_synthetic_thumbnail_with_energy(48, 57),
  });
  thumbnails.push_back(KeyframeThumbnail{
      .timestamp_ms = 25000,
      .image = create_synthetic_thumbnail_with_energy(48, 57),
  });
  thumbnails.push_back(KeyframeThumbnail{
      .timestamp_ms = 42000,
      .image = create_synthetic_bimodal_thumbnail(41, 45, 49, 54, 105, 215),
  });
  source.set_keyframe_thumbnails(std::move(thumbnails));

  source.add_frame(Frame{
      .timestamp_ms = 10000,
      .image = ImageBuffer(1920, 1080, PixelFormat::RGB24),
  });
  source.add_frame(Frame{
      .timestamp_ms = 25000,
      .image = ImageBuffer(1920, 1080, PixelFormat::RGB24),
  });
  source.add_frame(Frame{
      .timestamp_ms = 42000,
      .image = ImageBuffer(1920, 1080, PixelFormat::RGB24),
  });

  // Stage 2 crops bottom 50% (y_offset = 540).
  MockOcrEngine ocr;
  ocr.set_sequence(std::vector<OcrResult>{
      OcrResult{
          .lines = {
              OcrLine{
                  .text = "单行对白第一帧",
                  .confidence = 0.95,
                  .box = OcrCropBox{660, 340, 600, 42},
              },
          },
      },
      OcrResult{
          .lines = {
              OcrLine{
                  .text = "单行对白第二帧",
                  .confidence = 0.94,
                  .box = OcrCropBox{640, 340, 640, 42},
              },
          },
      },
      OcrResult{
          .lines = {
              OcrLine{
                  .text = "双行字幕上行文本",
                  .confidence = 0.93,
                  .box = OcrCropBox{680, 286, 560, 40},
              },
              OcrLine{
                  .text = "双行字幕下行文本",
                  .confidence = 0.95,
                  .box = OcrCropBox{650, 340, 620, 42},
              },
          },
      },
  });

  AutoRegionOptions opts{
      .script = "auto",
      .bottom_ratio = 0.45,
  };

  auto result = detect_auto_region(source, ocr, "dual_line.mp4", opts);

  REQUIRE(result.detected);
  CHECK(ocr.call_count() == 3);
  CHECK(result.region_box.y <= 826);
  CHECK(result.region_box.y + result.region_box.height >= 922);
  CHECK(result.profile.height >= 38);
  CHECK(result.profile.height <= 45);
  CHECK(result.preview_text.find("双行字幕上行文本") != std::string::npos);
}

