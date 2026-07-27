#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include "sublift/mock_ocr.hpp"
#include "sublift/models.hpp"
#include "sublift/pipeline.hpp"

#include "pipeline_test_helpers.hpp"

using namespace sublift::test;
using sublift::Frame;
using sublift::FrameLocalBox;
using sublift::IDetector;
using sublift::Region;

namespace {

// Detector that counts detect() calls - verifies cancel suppresses re-detect.
class CountingDetector final : public IDetector {
 public:
  explicit CountingDetector(FrameLocalBox box) : box_(box) {}
  std::optional<Region> detect(const Frame&) override {
    ++calls_;
    return Region{box_};
  }
  int calls() const noexcept { return calls_; }

 private:
  FrameLocalBox box_;
  int calls_{0};
};

}  // namespace

// ===========================================================================
// finalize: no open segment
// ===========================================================================

TEST_CASE("finalize: no open segment returns merged closed_entries_",
          "[pipeline][finalize]") {
  auto det = full_detector();
  auto ocr = sublift::MockOcrEngine("x", 1.0);
  sublift::Pipeline pipe(det, ocr, base_cfg());
  auto result = pipe.finalize();
  REQUIRE(result.empty());
}

TEST_CASE("finalize: returns merged entries from prior ocr_segment",
          "[pipeline][finalize]") {
  auto cfg = base_cfg();
  cfg.enable_line_select = false;
  cfg.confidence_threshold = 0.0;
  cfg.min_duration_ms = 0;
  auto det = full_detector();
  auto ocr = sublift::MockOcrEngine(std::vector<sublift::OcrResult>{
      sublift::OcrResult{"hello", 0.9, {}},
      sublift::OcrResult{"world", 0.9, {}}});
  sublift::Pipeline pipe(det, ocr, cfg);
  (void)pipe.feed(empty_frame(0));  // detect region
  sublift::SegmentEvent e1{100, 300, empty_frame(200), {}};
  sublift::SegmentEvent e2{400, 600, empty_frame(500), {}};
  (void)pipe.ocr_segment(e1);
  (void)pipe.ocr_segment(e2);
  auto result = pipe.finalize();
  REQUIRE(result.size() == 2);
  REQUIRE(result[0].text == "hello");
  REQUIRE(result[1].text == "world");
}

// ===========================================================================
// finalize: trailing open segment
// ===========================================================================

TEST_CASE("finalize: closes trailing open segment and OCRs it",
          "[pipeline][finalize]") {
  auto cfg = base_cfg();
  cfg.enable_line_select = false;
  cfg.confidence_threshold = 0.0;
  cfg.min_duration_ms = 0;
  auto det = full_detector();
  // Sequence or fixed both advance call_count(); sequence used for a single
  // scripted result (and exhaustion if over-called).
  auto ocr = sublift::MockOcrEngine(std::vector<sublift::OcrResult>{
      sublift::OcrResult{"tail", 0.9, {}}});
  sublift::Pipeline pipe(det, ocr, cfg);
  (void)pipe.feed(empty_frame(0));  // detect region
  (void)pipe.feed(subA(200));       // IN -> open segment @200
  (void)pipe.feed(subA(400));       // stable (delay countdown)
  // No OUT emitted; finalize must close the trailing open segment.
  auto result = pipe.finalize();
  REQUIRE(result.size() == 1);
  REQUIRE(result[0].text == "tail");
  REQUIRE(result[0].start_ms == 200);
  REQUIRE(result[0].end_ms == 400);  // last_timestamp_ms_
  REQUIRE(ocr.call_count() == 1);
}

TEST_CASE("finalize: idempotent (closed_entries_ not cleared)",
          "[pipeline][finalize]") {
  auto cfg = base_cfg();
  cfg.enable_line_select = false;
  cfg.confidence_threshold = 0.0;
  cfg.min_duration_ms = 0;
  auto det = full_detector();
  auto ocr = sublift::MockOcrEngine(std::vector<sublift::OcrResult>{
      sublift::OcrResult{"tail", 0.9, {}}, sublift::OcrResult{"tail", 0.9, {}}});
  sublift::Pipeline pipe(det, ocr, cfg);
  (void)pipe.feed(empty_frame(0));
  (void)pipe.feed(subA(200));
  (void)pipe.feed(subA(400));
  auto first = pipe.finalize();
  REQUIRE(first.size() == 1);
  REQUIRE(first[0].text == "tail");
  auto second = pipe.finalize();  // open segment already gone
  REQUIRE(second.size() == 1);
  REQUIRE(second[0].text == "tail");
  REQUIRE(ocr.call_count() == 1);  // second finalize does not re-OCR
}

// ===========================================================================
// cancel: state reset + permanent feed rejection
// ===========================================================================

TEST_CASE("cancel: feed returns nullopt and is a no-op",
          "[pipeline][cancel]") {
  auto det = full_detector();
  auto ocr = sublift::MockOcrEngine("x", 1.0);
  sublift::Pipeline pipe(det, ocr, base_cfg());
  (void)pipe.feed(empty_frame(0));
  (void)pipe.feed(subA(200));
  pipe.cancel();
  REQUIRE_FALSE(pipe.feed(subA(400)).has_value());
  REQUIRE_FALSE(pipe.feed(empty_frame(600)).has_value());
}

TEST_CASE("cancel: resets processed_count to 0", "[pipeline][cancel]") {
  auto det = full_detector();
  auto ocr = sublift::MockOcrEngine("x", 1.0);
  sublift::Pipeline pipe(det, ocr, base_cfg());
  (void)pipe.feed(empty_frame(0));
  (void)pipe.feed(subA(200));
  REQUIRE(pipe.processed_count() == 2);
  pipe.cancel();
  REQUIRE(pipe.processed_count() == 0);
  (void)pipe.feed(subA(400));  // rejected
  REQUIRE(pipe.processed_count() == 0);
}

TEST_CASE("cancel: region_ cleared, feed does not re-detect",
          "[pipeline][cancel]") {
  // Python cancel is permanent: feed() returns nullopt forever (core.py 248).
  CountingDetector det(FrameLocalBox{0, 0, kW, kH});
  auto ocr = sublift::MockOcrEngine("x", 1.0);
  sublift::Pipeline pipe(det, ocr, base_cfg());
  (void)pipe.feed(empty_frame(0));  // detect() called once
  REQUIRE(det.calls() == 1);
  pipe.cancel();
  (void)pipe.feed(empty_frame(100));  // rejected, no re-detect
  REQUIRE(det.calls() == 1);
  // region_ is empty -> ocr_segment takes the region-None branch.
  sublift::SegmentEvent e{100, 300, empty_frame(200), {}};
  auto entry = pipe.ocr_segment(e);
  REQUIRE(entry.text.empty());
  REQUIRE(entry.confidence == 0.0);
  REQUIRE(ocr.call_count() == 0);
}

TEST_CASE("cancel: subtitle_profile_ reset to config default",
          "[pipeline][cancel]") {
  // base_cfg() leaves subtitle_profile = nullopt (config default).
  auto cfg = base_cfg();
  REQUIRE_FALSE(cfg.subtitle_profile.has_value());
  auto det = full_detector();
  auto ocr = sublift::MockOcrEngine("x", 1.0);
  sublift::Pipeline pipe(det, ocr, cfg);
  (void)pipe.feed(empty_frame(0));  // detect -> ensure_subtitle_profile sets it
  REQUIRE(pipe.subtitle_profile().has_value());
  pipe.cancel();
  REQUIRE_FALSE(pipe.subtitle_profile().has_value());
}

TEST_CASE("cancel: ocr_segment returns empty entry (region-None branch)",
          "[pipeline][cancel]") {
  auto det = full_detector();
  auto ocr = sublift::MockOcrEngine("x", 1.0);
  sublift::Pipeline pipe(det, ocr, base_cfg());
  pipe.cancel();  // no feed at all
  sublift::SegmentEvent e{100, 300, empty_frame(200), {}};
  auto entry = pipe.ocr_segment(e);
  REQUIRE(entry.text.empty());
  REQUIRE(entry.confidence == 0.0);
  REQUIRE(entry.start_ms == 100);
  REQUIRE(entry.end_ms == 300);
  REQUIRE(ocr.call_count() == 0);
}

// ===========================================================================
// finalize + cancel interaction
// ===========================================================================

TEST_CASE("finalize then cancel: feed rejected, second finalize empty",
          "[pipeline][finalize][cancel]") {
  auto cfg = base_cfg();
  cfg.enable_line_select = false;
  cfg.confidence_threshold = 0.0;
  cfg.min_duration_ms = 0;
  auto det = full_detector();
  auto ocr = sublift::MockOcrEngine("tail", 0.9);
  sublift::Pipeline pipe(det, ocr, cfg);
  (void)pipe.feed(empty_frame(0));
  (void)pipe.feed(subA(200));
  (void)pipe.feed(subA(400));
  auto result = pipe.finalize();
  REQUIRE(result.size() == 1);
  pipe.cancel();
  REQUIRE_FALSE(pipe.feed(subA(600)).has_value());
  auto after = pipe.finalize();  // closed_entries_ cleared by cancel
  REQUIRE(after.empty());
}

TEST_CASE("cancel then finalize: returns empty (closed_entries_ cleared)",
          "[pipeline][finalize][cancel]") {
  auto cfg = base_cfg();
  cfg.enable_line_select = false;
  cfg.confidence_threshold = 0.0;
  auto det = full_detector();
  auto ocr = sublift::MockOcrEngine("x", 1.0);
  sublift::Pipeline pipe(det, ocr, cfg);
  (void)pipe.feed(empty_frame(0));
  (void)pipe.feed(subA(200));
  pipe.cancel();
  auto result = pipe.finalize();  // open segment cleared, closed_entries_ empty
  REQUIRE(result.empty());
}
