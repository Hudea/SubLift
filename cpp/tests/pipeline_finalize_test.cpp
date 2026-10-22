#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdlib>
#include <string>
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

// Scoped env-var override so D03 suppression tests are hermetic (Catch2 runs
// all TEST_CASEs in one process).
class ScopedEnv {
 public:
  ScopedEnv(const char* name, const char* value) : name_(name) {
    const char* old = std::getenv(name);
    if (old != nullptr) {
      had_old_ = true;
      old_ = old;
    }
    setenv(name, value, 1);
  }
  ~ScopedEnv() {
    if (had_old_) {
      setenv(name_.c_str(), old_.c_str(), 1);
    } else {
      unsetenv(name_.c_str());
    }
  }

  ScopedEnv(const ScopedEnv&) = delete;
  ScopedEnv& operator=(const ScopedEnv&) = delete;

 private:
  std::string name_;
  std::string old_;
  bool had_old_{false};
};

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

// ===========================================================================
// Phase 21 D03: entry-level watermark suppression in finalize
// ===========================================================================

namespace {

// Raw OCR sequence for a 32s clip where the same scrolling watermark is
// re-read in the dialogue gaps: 6 watermark variants + 1 real cue.
std::vector<sublift::OcrResult> watermark_ocr_sequence() {
  const auto watermark = [](const char* text) {
    return sublift::OcrResult{text, 0.95, {}};
  };
  return {
      watermark("暖暖的猫"),
      sublift::OcrResult{"正常字幕内容", 0.95, {}},
      watermark("暖暖的猫98"),
      watermark("暖暖的猫"),
      watermark("暖暖的猫98Tang.COM"),
      watermark("暖暖的猫"),
      watermark("暖的猫"),
      watermark("暖暖的猫98"),
  };
}

// Feed enough frames to establish a 32s video duration, then OCR the segment
// events. Legacy path (enable_line_select=false) keeps one OCR call per event.
void run_watermark_pipeline(sublift::Pipeline& pipe) {
  (void)pipe.feed(empty_frame(0));      // detect region, last_ts = 0
  (void)pipe.feed(empty_frame(32000));  // video-duration proxy for D03
  // SegmentEvent holds owned Frames (move-only), so build via push_back of a
  // temporary instead of an initializer_list.
  std::vector<sublift::SegmentEvent> events;
  events.reserve(8);
  const auto add_event = [&events](std::int64_t start, std::int64_t end,
                                   std::int64_t anchor_ts) {
    events.push_back(sublift::SegmentEvent{start, end, empty_frame(anchor_ts), {}});
  };
  add_event(0, 2000, 1000);
  add_event(2000, 4000, 3000);
  add_event(4000, 6000, 5000);
  add_event(8000, 10000, 9000);
  add_event(12000, 14000, 13000);
  add_event(16000, 18000, 17000);
  add_event(20000, 22000, 21000);
  add_event(24000, 26000, 25000);
  for (const auto& event : events) {
    (void)pipe.ocr_segment(event);
  }
}

sublift::Config watermark_cfg() {
  auto cfg = base_cfg();
  cfg.enable_line_select = false;
  cfg.confidence_threshold = 0.0;
  cfg.min_duration_ms = 0;
  cfg.drop_empty_text = false;
  return cfg;
}

}  // namespace

TEST_CASE("finalize suppresses persistent watermark family, keeps dialogue",
          "[watermark]") {
  ScopedEnv env("SUBLIFT_WATERMARK_FILTER", "1");  // default ON, explicit
  auto det = full_detector();
  auto ocr = sublift::MockOcrEngine(watermark_ocr_sequence());
  sublift::Pipeline pipe(det, ocr, watermark_cfg());
  run_watermark_pipeline(pipe);
  const auto result = pipe.finalize();
  REQUIRE(result.size() == 1);
  REQUIRE(result[0].text == "正常字幕内容");
  REQUIRE(result[0].start_ms == 2000);
  REQUIRE(result[0].end_ms == 4000);
}

TEST_CASE("SUBLIFT_WATERMARK_FILTER=0 disables entry suppression",
          "[watermark]") {
  ScopedEnv env("SUBLIFT_WATERMARK_FILTER", "0");
  auto det = full_detector();
  auto ocr = sublift::MockOcrEngine(watermark_ocr_sequence());
  sublift::Pipeline pipe(det, ocr, watermark_cfg());
  run_watermark_pipeline(pipe);
  const auto result = pipe.finalize();
  // 7 watermark cues + 1 real cue; adjacent watermark texts are farther apart
  // than merge_gap_ms so they stay separate.
  REQUIRE(result.size() == 8);
  int watermark_entries = 0;
  for (const auto& entry : result) {
    if (entry.text.find("暖暖的猫") != std::string::npos ||
        entry.text.find("暖的猫") != std::string::npos ||
        entry.text.find("98Tang") != std::string::npos) {
      ++watermark_entries;
    }
  }
  REQUIRE(watermark_entries == 7);
}

TEST_CASE("finalize leaves normal repeated short dialogue alone", "[watermark]") {
  ScopedEnv env("SUBLIFT_WATERMARK_FILTER", "1");
  auto cfg = watermark_cfg();
  auto det = full_detector();
  // 3 x "谢谢" over a 32s clip: below the D03 family/coverage thresholds.
  auto ocr = sublift::MockOcrEngine(std::vector<sublift::OcrResult>{
      sublift::OcrResult{"谢谢", 0.95, {}},
      sublift::OcrResult{"谢谢", 0.95, {}},
      sublift::OcrResult{"谢谢", 0.95, {}},
  });
  sublift::Pipeline pipe(det, ocr, cfg);
  (void)pipe.feed(empty_frame(0));
  (void)pipe.feed(empty_frame(32000));
  for (int i = 0; i < 3; ++i) {
    (void)pipe.ocr_segment(sublift::SegmentEvent{
        static_cast<std::int64_t>(i) * 5000,
        static_cast<std::int64_t>(i) * 5000 + 2000, empty_frame(1000), {}});
  }
  const auto result = pipe.finalize();
  REQUIRE(result.size() == 3);
  for (const auto& entry : result) {
    REQUIRE(entry.text == "谢谢");
  }
}
