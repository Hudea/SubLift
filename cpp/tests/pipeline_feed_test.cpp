#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

#include "sublift/image.hpp"
#include "sublift/mock_ocr.hpp"
#include "sublift/pipeline.hpp"

#include "pipeline_test_helpers.hpp"

using namespace sublift::test;

TEST_CASE("feed returns nullopt when detector has no region", "[pipeline]") {
  FirstNullDetector det;
  sublift::MockOcrEngine ocr("x", 1.0);
  sublift::Pipeline pipe(det, ocr, base_cfg());

  REQUIRE_FALSE(pipe.feed(empty_frame(0)).has_value());
  REQUIRE(pipe.processed_count() == 1);
}

TEST_CASE("single IN opens segment, returns nullopt", "[pipeline]") {
  auto det = full_detector();
  sublift::MockOcrEngine ocr("x", 1.0);
  sublift::Pipeline pipe(det, ocr, base_cfg());

  REQUIRE_FALSE(pipe.feed(empty_frame(0)).has_value());
  REQUIRE_FALSE(pipe.feed(subA(200)).has_value());
  REQUIRE(pipe.processed_count() == 2);
}

TEST_CASE("IN then OUT emits SegmentEvent (delay not reached -> first anchor)",
          "[pipeline]") {
  auto det = full_detector();
  sublift::MockOcrEngine ocr("x", 1.0);
  sublift::Pipeline pipe(det, ocr, base_cfg());

  REQUIRE_FALSE(pipe.feed(empty_frame(0)).has_value());
  REQUIRE_FALSE(pipe.feed(subA(200)).has_value());  // IN
  REQUIRE_FALSE(pipe.feed(subA(400)).has_value());  // stable
  auto ev = pipe.feed(empty_frame(600));            // OUT
  REQUIRE(ev.has_value());
  REQUIRE(ev->start_ms == 200);
  REQUIRE(ev->end_ms == 600);
  REQUIRE(ev->anchor_frame.has_value());
  REQUIRE(ev->anchor_frame->timestamp_ms == 200);  // delay=2 not reached -> first
  REQUIRE(fallback_ts(*ev) == std::vector<std::int64_t>{400, 600});
  REQUIRE(ocr.call_count() == 0);
}

TEST_CASE("feed makes zero OCR calls", "[pipeline]") {
  auto det = full_detector();
  sublift::MockOcrEngine ocr("x", 1.0);
  sublift::Pipeline pipe(det, ocr, base_cfg());

  (void)pipe.feed(empty_frame(0));
  (void)pipe.feed(subA(200));
  (void)pipe.feed(subA(400));
  (void)pipe.feed(empty_frame(600));
  REQUIRE(ocr.call_count() == 0);
}

TEST_CASE("delay=0 locks anchor on first frame", "[pipeline]") {
  auto det = full_detector();
  sublift::MockOcrEngine ocr("x", 1.0);
  auto cfg = base_cfg();
  cfg.ocr_anchor_delay_frames = 0;
  sublift::Pipeline pipe(det, ocr, cfg);

  REQUIRE_FALSE(pipe.feed(empty_frame(0)).has_value());
  REQUIRE_FALSE(pipe.feed(subA(200)).has_value());  // IN, delay=0 -> lock@200
  REQUIRE_FALSE(pipe.feed(subA(400)).has_value());  // stable
  auto ev = pipe.feed(empty_frame(600));            // OUT
  REQUIRE(ev.has_value());
  REQUIRE(ev->start_ms == 200);
  REQUIRE(ev->end_ms == 600);
  REQUIRE(ev->anchor_frame->timestamp_ms == 200);  // locked at open
  REQUIRE(fallback_ts(*ev) == std::vector<std::int64_t>{400, 600});
}

TEST_CASE("delay=2 locks anchor on 3rd stable frame", "[pipeline]") {
  auto det = full_detector();
  sublift::MockOcrEngine ocr("x", 1.0);
  sublift::Pipeline pipe(det, ocr, base_cfg());  // delay=2, consensus=4

  REQUIRE_FALSE(pipe.feed(empty_frame(0)).has_value());
  REQUIRE_FALSE(pipe.feed(subA(200)).has_value());  // IN; pending=2
  REQUIRE_FALSE(pipe.feed(subA(400)).has_value());  // pending 2->1
  REQUIRE_FALSE(pipe.feed(subA(600)).has_value());  // pending 1->0 -> lock@600
  REQUIRE_FALSE(pipe.feed(subA(800)).has_value());  // stable@800
  auto ev = pipe.feed(empty_frame(1000));           // OUT
  REQUIRE(ev.has_value());
  REQUIRE(ev->start_ms == 200);
  REQUIRE(ev->end_ms == 1000);
  REQUIRE(ev->anchor_frame->timestamp_ms == 600);  // delayed anchor
  REQUIRE(fallback_ts(*ev) == std::vector<std::int64_t>{200, 400, 800, 1000});
}

TEST_CASE("CHANGE closes old segment and opens new", "[pipeline]") {
  auto det = full_detector();
  sublift::MockOcrEngine ocr("x", 1.0);
  auto cfg = base_cfg();
  cfg.change_point.change_threshold = 5;  // ham(subA,subB)=8 > 5 -> CHANGE
  sublift::Pipeline pipe(det, ocr, cfg);

  REQUIRE_FALSE(pipe.feed(empty_frame(0)).has_value());
  REQUIRE_FALSE(pipe.feed(subA(200)).has_value());  // IN
  REQUIRE_FALSE(pipe.feed(subB(400)).has_value());  // candidate (not yet stable)
  auto change_ev = pipe.feed(subB(600));            // CHANGE@400
  REQUIRE(change_ev.has_value());
  REQUIRE(change_ev->start_ms == 200);
  REQUIRE(change_ev->end_ms == 400);
  REQUIRE(change_ev->anchor_frame->timestamp_ms == 200);  // first (delay not reached)
  REQUIRE(fallback_ts(*change_ev) == std::vector<std::int64_t>{400});

  auto out_ev = pipe.feed(empty_frame(800));  // OUT of new segment
  REQUIRE(out_ev.has_value());
  // New segment opened at the CHANGE timestamp (400); its first frame is the
  // current frame@600, so anchor=600.
  REQUIRE(out_ev->start_ms == 400);
  REQUIRE(out_ev->end_ms == 800);
  REQUIRE(out_ev->anchor_frame->timestamp_ms == 600);  // first of new segment
  REQUIRE(fallback_ts(*out_ev) == std::vector<std::int64_t>{800});
  REQUIRE(ocr.call_count() == 0);
}

TEST_CASE("cancel makes feed a no-op", "[pipeline]") {
  auto det = full_detector();
  sublift::MockOcrEngine ocr("x", 1.0);
  sublift::Pipeline pipe(det, ocr, base_cfg());

  (void)pipe.feed(empty_frame(0));
  (void)pipe.feed(subA(200));
  pipe.cancel();
  // Python cancel resets processed_count to 0 and rejects all future feed
  // calls (core.py 434-446). The full reset semantics are exercised by
  // pipeline_finalize_test.cpp; here we only assert the feed rejection.
  REQUIRE_FALSE(pipe.feed(subA(400)).has_value());
  REQUIRE(pipe.processed_count() == 0);
}

TEST_CASE("ROI sub-region crop path works", "[pipeline]") {
  // Sub-region [4,4,120,56] (genuine ROI, not full-frame passthrough); the
  // subA band [48,80)x[44,60) falls fully inside it. Calibrated vs Python
  // oracle: this box yields fg_ratio>0.01 so an IN/OUT pair fires.
  sublift::FixedRegionDetector det(sublift::FrameLocalBox{4, 4, 120, 56});
  sublift::MockOcrEngine ocr("x", 1.0);
  sublift::Pipeline pipe(det, ocr, base_cfg());

  (void)pipe.feed(empty_frame(0));
  (void)pipe.feed(subA(200));  // IN via ROI crop
  auto ev = pipe.feed(empty_frame(600));  // OUT
  REQUIRE(ev.has_value());
  REQUIRE(ev->start_ms == 200);
  REQUIRE(ev->end_ms == 600);
  REQUIRE(ocr.call_count() == 0);
}

TEST_CASE("chromatic cyan band: RGB→BGR quirk required for IN/OUT",
          "[pipeline]") {
  // subCyan R≠B: missing convert_rgb_to_bgr would yield fg=0 (no IN).
  auto det = full_detector();
  sublift::MockOcrEngine ocr("x", 1.0);
  sublift::Pipeline pipe(det, ocr, base_cfg());

  REQUIRE_FALSE(pipe.feed(empty_frame(0)).has_value());
  REQUIRE_FALSE(pipe.feed(subCyan(200)).has_value());  // IN
  REQUIRE_FALSE(pipe.feed(subCyan(400)).has_value());  // stable
  auto ev = pipe.feed(empty_frame(600));               // OUT
  REQUIRE(ev.has_value());
  REQUIRE(ev->start_ms == 200);
  REQUIRE(ev->end_ms == 600);
  REQUIRE(ev->anchor_frame.has_value());
  REQUIRE(ev->anchor_frame->timestamp_ms == 200);
  REQUIRE(ocr.call_count() == 0);
}

TEST_CASE("SegmentEvent frames remain valid after source wiped",
          "[pipeline]") {
  // P0 ownership: feed deep-copies retained frames so OCR after mutating or
  // destroying caller frames still sees original pixels.
  auto det = full_detector();
  sublift::MockOcrEngine ocr("owned", 0.95);
  auto cfg = base_cfg();
  cfg.enable_line_select = false;
  cfg.confidence_threshold = 0.0;
  cfg.min_duration_ms = 0;
  sublift::Pipeline pipe(det, ocr, cfg);

  sublift::Frame f0 = empty_frame(0);
  sublift::Frame f_in = subA(200);
  sublift::Frame f_stable = subA(400);
  sublift::Frame f_out = empty_frame(600);

  REQUIRE_FALSE(pipe.feed(f0).has_value());
  REQUIRE_FALSE(pipe.feed(f_in).has_value());
  REQUIRE_FALSE(pipe.feed(f_stable).has_value());

  // Wipe caller-owned pixels after retention / before OUT.
  const std::size_t n0 =
      static_cast<std::size_t>(f_in.image.height()) *
      static_cast<std::size_t>(f_in.image.stride_bytes());
  std::fill_n(f_in.image.data(), n0, static_cast<std::uint8_t>(0));
  std::fill_n(f_stable.image.data(), n0, static_cast<std::uint8_t>(0));
  std::fill_n(f0.image.data(), n0, static_cast<std::uint8_t>(0));

  auto ev = pipe.feed(f_out);
  REQUIRE(ev.has_value());
  std::fill_n(f_out.image.data(), n0, static_cast<std::uint8_t>(0));

  REQUIRE(ev->anchor_frame.has_value());
  // Anchor is a deep copy of subA@200: bg pixel still kBg, not wiped zero.
  REQUIRE(ev->anchor_frame->image.data()[0] == kBg);
  REQUIRE(ev->anchor_frame->image.data()[1] == kBg);
  REQUIRE(ev->anchor_frame->image.data()[2] == kBg);
  // Band interior (x=64,y=52) still bright.
  {
    const auto& img = ev->anchor_frame->image;
    const std::uint8_t* row =
        img.data() + static_cast<std::size_t>(52) * img.stride_bytes();
    REQUIRE(row[64 * 3 + 0] == kFg);
  }
  for (const auto& fb : ev->fallback_frames) {
    REQUIRE_FALSE(fb.image.empty());
    REQUIRE(fb.image.data()[0] == kBg);
  }

  auto entry = pipe.ocr_segment(*ev);
  REQUIRE(entry.text == "owned");
  REQUIRE(entry.confidence == 0.95);
  REQUIRE(ocr.call_count() == 1);
}

TEST_CASE("feed rejects non-RGB24 crop", "[pipeline]") {
  auto det = full_detector();
  sublift::MockOcrEngine ocr("x", 1.0);
  sublift::Pipeline pipe(det, ocr, base_cfg());
  sublift::Frame gray{
      0, sublift::ImageBuffer(kW, kH, sublift::PixelFormat::Gray8)};
  REQUIRE_THROWS_AS(pipe.feed(gray), std::invalid_argument);
  REQUIRE(ocr.call_count() == 0);
}
