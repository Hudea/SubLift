#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <vector>

#include "sublift/mock_ocr.hpp"
#include "sublift/models.hpp"
#include "sublift/pipeline.hpp"

#include "pipeline_test_helpers.hpp"

using namespace sublift::test;

namespace {

// Build a SegmentEvent with owned frames (path B: hand-constructed, precise
// control over anchor / fallback ts without driving feed).
sublift::SegmentEvent make_event(std::int64_t start, std::int64_t end,
                                 std::optional<sublift::Frame> anchor,
                                 std::vector<sublift::Frame> fallbacks) {
  return sublift::SegmentEvent{start, end, std::move(anchor),
                               std::move(fallbacks)};
}

// Build a fallback list by moving rvalue frames (Frame is move-only, so an
// initializer_list - which only exposes const access - cannot be used).
std::vector<sublift::Frame> frames() { return {}; }
template <typename... Ts>
std::vector<sublift::Frame> frames(sublift::Frame first, Ts... rest) {
  std::vector<sublift::Frame> v;
  v.push_back(std::move(first));
  (v.push_back(std::move(rest)), ...);
  return v;
}

// OCR result carrying one line (exercises the line_select path).
sublift::OcrResult line_result(const std::string& text, double conf) {
  std::vector<sublift::OcrLine> lines{
      sublift::OcrLine{text, conf, sublift::OcrCropBox{0, 44, kW, 16}}};
  return sublift::OcrResult::from_lines(lines);
}

// Sequence mock repeating the same result. Fixed mode also advances
// call_count(); sequence is still preferred when distinct per-call results
// are needed. `n` must cover the expected call count.
sublift::MockOcrEngine seq(sublift::OcrResult r, std::size_t n = 5) {
  return sublift::MockOcrEngine(std::vector<sublift::OcrResult>(n, r));
}

}  // namespace

// ===========================================================================
// region_ None branch
// ===========================================================================

TEST_CASE("ocr_segment returns empty entry when no region", "[pipeline][ocr]") {
  NeverDetectDetector det;
  auto ocr = seq(sublift::OcrResult{"x", 1.0, {}});
  sublift::Pipeline pipe(det, ocr, base_cfg());
  auto e = pipe.ocr_segment(make_event(100, 300, empty_frame(200), {}));
  REQUIRE(e.text.empty());
  REQUIRE(e.confidence == 0.0);
  REQUIRE(e.start_ms == 100);
  REQUIRE(e.end_ms == 300);
  REQUIRE(ocr.call_count() == 0);
}

// ===========================================================================
// legacy path (enable_line_select = false)
// ===========================================================================

TEST_CASE("ocr legacy: anchor text adopted", "[pipeline][ocr]") {
  auto cfg = base_cfg();
  cfg.enable_line_select = false;
  auto det = full_detector();
  auto ocr = seq(sublift::OcrResult{"hello", 0.9, {}});
  sublift::Pipeline pipe(det, ocr, cfg);
  (void)pipe.feed(empty_frame(0));
  auto e = pipe.ocr_segment(make_event(100, 300, empty_frame(200), {}));
  REQUIRE(e.text == "hello");
  REQUIRE(e.confidence == 0.9);
  REQUIRE(ocr.call_count() == 1);
}

TEST_CASE("ocr legacy: empty anchor falls back to next frame", "[pipeline][ocr]") {
  auto cfg = base_cfg();
  cfg.enable_line_select = false;
  auto det = full_detector();
  auto ocr = sublift::MockOcrEngine(std::vector<sublift::OcrResult>{
      sublift::OcrResult{"", 0.0, {}}, sublift::OcrResult{"world", 0.9, {}}});
  sublift::Pipeline pipe(det, ocr, cfg);
  (void)pipe.feed(empty_frame(0));
  auto e = pipe.ocr_segment(
      make_event(100, 300, empty_frame(200), frames(empty_frame(400))));
  REQUIRE(e.text == "world");
  REQUIRE(e.confidence == 0.9);
  REQUIRE(ocr.call_count() == 2);
}

TEST_CASE("ocr legacy: sub-threshold confidence blanks text (conf kept)",
          "[pipeline][ocr]") {
  auto cfg = base_cfg();
  cfg.enable_line_select = false;
  cfg.confidence_threshold = 0.5;
  auto det = full_detector();
  auto ocr = seq(sublift::OcrResult{"low", 0.3, {}});
  sublift::Pipeline pipe(det, ocr, cfg);
  (void)pipe.feed(empty_frame(0));
  auto e = pipe.ocr_segment(make_event(100, 300, empty_frame(200), {}));
  REQUIRE(e.text.empty());
  REQUIRE(e.confidence == 0.3);  // confidence preserved
  REQUIRE(ocr.call_count() == 1);
}

TEST_CASE("ocr legacy: fallback with higher confidence wins", "[pipeline][ocr]") {
  auto cfg = base_cfg();
  cfg.enable_line_select = false;
  cfg.confidence_threshold = 0.5;
  auto det = full_detector();
  auto ocr = sublift::MockOcrEngine(std::vector<sublift::OcrResult>{
      sublift::OcrResult{"a", 0.3, {}}, sublift::OcrResult{"b", 0.8, {}}});
  sublift::Pipeline pipe(det, ocr, cfg);
  (void)pipe.feed(empty_frame(0));
  auto e = pipe.ocr_segment(
      make_event(100, 300, empty_frame(200), frames(empty_frame(400))));
  REQUIRE(e.text == "b");
  REQUIRE(e.confidence == 0.8);
  REQUIRE(ocr.call_count() == 2);
}

TEST_CASE("ocr legacy: seen_ts dedup skips same-timestamp fallback",
          "[pipeline][ocr]") {
  auto cfg = base_cfg();
  cfg.enable_line_select = false;
  auto det = full_detector();
  // anchor (ts=200) is empty -> retry. fallback at ts=200 is dedup-skipped;
  // fallback at ts=400 consumes the 2nd result.
  auto ocr = sublift::MockOcrEngine(std::vector<sublift::OcrResult>{
      sublift::OcrResult{"", 0.0, {}}, sublift::OcrResult{"world", 0.9, {}},
      sublift::OcrResult{"unused", 0.9, {}}});
  sublift::Pipeline pipe(det, ocr, cfg);
  (void)pipe.feed(empty_frame(0));
  auto e = pipe.ocr_segment(make_event(
      100, 300, empty_frame(200), frames(empty_frame(200), empty_frame(400))));
  REQUIRE(e.text == "world");
  REQUIRE(e.confidence == 0.9);
  REQUIRE(ocr.call_count() == 2);  // anchor + ts=400 fb (ts=200 fb skipped)
}

TEST_CASE("ocr legacy: sequence exhaustion throws", "[pipeline][ocr]") {
  auto cfg = base_cfg();
  cfg.enable_line_select = false;
  cfg.confidence_threshold = 0.5;
  auto det = full_detector();
  // 1 result; anchor retries, fallback needs a 2nd call -> out_of_range.
  auto ocr = sublift::MockOcrEngine(
      std::vector<sublift::OcrResult>{sublift::OcrResult{"a", 0.3, {}}});
  sublift::Pipeline pipe(det, ocr, cfg);
  (void)pipe.feed(empty_frame(0));
  REQUIRE_THROWS_AS(
      pipe.ocr_segment(
          make_event(100, 300, empty_frame(200), frames(empty_frame(400)))),
      std::out_of_range);
}

// ===========================================================================
// line_select path (enable_line_select = true, CJK profile)
// ===========================================================================

TEST_CASE("ocr line_select: single high-confidence early stop", "[pipeline][ocr]") {
  auto cfg = base_cfg();
  cfg.subtitle_script = "cjk";
  auto det = full_detector();
  auto ocr = seq(line_result("你好", 0.9));
  sublift::Pipeline pipe(det, ocr, cfg);
  (void)pipe.feed(empty_frame(0));
  auto e = pipe.ocr_segment(make_event(100, 300, empty_frame(200), {}));
  REQUIRE(e.text == "你好");
  REQUIRE(e.confidence == 0.9);
  REQUIRE(ocr.call_count() == 1);
}

TEST_CASE("ocr line_select: two-frame consensus", "[pipeline][ocr]") {
  auto cfg = base_cfg();
  cfg.subtitle_script = "cjk";
  auto det = full_detector();
  auto ocr = seq(line_result("你好", 0.3));  // < threshold, >= low_conf
  sublift::Pipeline pipe(det, ocr, cfg);
  (void)pipe.feed(empty_frame(0));
  auto e = pipe.ocr_segment(
      make_event(100, 300, empty_frame(200), frames(empty_frame(400))));
  REQUIRE(e.text == "你好");
  REQUIRE(e.confidence == 0.3);
  REQUIRE(ocr.call_count() == 2);
}

TEST_CASE("ocr line_select: no valid sample returns empty", "[pipeline][ocr]") {
  auto cfg = base_cfg();
  cfg.subtitle_script = "cjk";
  auto det = full_detector();
  auto ocr = seq(line_result("你好", 0.9));
  sublift::Pipeline pipe(det, ocr, cfg);
  (void)pipe.feed(empty_frame(0));
  auto e = pipe.ocr_segment(make_event(100, 300, std::nullopt, {}));
  REQUIRE(e.text.empty());
  REQUIRE(e.confidence == 0.0);
  REQUIRE(ocr.call_count() == 0);
}

TEST_CASE("ocr line_select: low-confidence single frame rejected (conf kept)",
          "[pipeline][ocr]") {
  auto cfg = base_cfg();
  cfg.subtitle_script = "cjk";
  auto det = full_detector();
  auto ocr = seq(line_result("你好", 0.2));  // < low_conf -> rejected
  sublift::Pipeline pipe(det, ocr, cfg);
  (void)pipe.feed(empty_frame(0));
  auto e = pipe.ocr_segment(make_event(100, 300, empty_frame(200), {}));
  REQUIRE(e.text.empty());
  REQUIRE(e.confidence == 0.2);  // confidence preserved
  REQUIRE(ocr.call_count() == 1);
}

TEST_CASE("ocr line_select: no-lines engine compat", "[pipeline][ocr]") {
  auto cfg = base_cfg();
  cfg.subtitle_script = "cjk";
  auto det = full_detector();
  auto ocr = seq(sublift::OcrResult{"你好", 0.7, {}});  // no lines
  sublift::Pipeline pipe(det, ocr, cfg);
  (void)pipe.feed(empty_frame(0));
  auto e = pipe.ocr_segment(make_event(100, 300, empty_frame(200), {}));
  REQUIRE(e.text == "你好");
  REQUIRE(e.confidence == 0.7);
  REQUIRE(ocr.call_count() == 1);
}

TEST_CASE("ocr line_select: mixed CJK+latin does not early-stop on one frame",
           "[pipeline][ocr]") {
  auto cfg = base_cfg();
  cfg.subtitle_script = "cjk";
  auto det = full_detector();
  // "你好World你好" stays mixed after cleanup -> single_high_confidence guard
  // (no early break on frame 1); two_frame_consensus breaks on frame 2.
  auto ocr = seq(line_result("你好World你好", 0.9));
  sublift::Pipeline pipe(det, ocr, cfg);
  (void)pipe.feed(empty_frame(0));
  auto e = pipe.ocr_segment(
      make_event(100, 300, empty_frame(200), frames(empty_frame(400))));
  REQUIRE(e.text == "你好World你好");
  REQUIRE(e.confidence == 0.9);
  REQUIRE(ocr.call_count() == 2);  // no early stop; pure CJK would be 1
}

// ===========================================================================
// P3 coverage: edge branches confirmed by review (parity with core.py)
// ===========================================================================

// legacy: non-empty fallback retained even when it still needs retry
// (core.py 580-581: fb_text.strip() and not text.strip() -> keep fb_text).
// anchor empty -> retry; fb non-empty but below threshold -> not break, but
// retained as the best non-empty sample; final conf<threshold blanks text.
TEST_CASE("ocr legacy: non-empty low-conf fallback retained then blanked",
          "[pipeline][ocr]") {
  auto cfg = base_cfg();
  cfg.enable_line_select = false;
  cfg.confidence_threshold = 0.5;
  auto det = full_detector();
  // anchor empty (retry); fb "noise" non-empty but conf=0.2 < threshold ->
  // retained as text, then blanked at the final threshold check.
  auto ocr = sublift::MockOcrEngine(std::vector<sublift::OcrResult>{
      sublift::OcrResult{"", 0.0, {}}, sublift::OcrResult{"noise", 0.2, {}}});
  sublift::Pipeline pipe(det, ocr, cfg);
  (void)pipe.feed(empty_frame(0));
  auto e = pipe.ocr_segment(
      make_event(100, 300, empty_frame(200), frames(empty_frame(400))));
  REQUIRE(e.text.empty());  // blanked by final threshold check
  REQUIRE(e.confidence == 0.2);
  REQUIRE(ocr.call_count() == 2);
}

// line_select: cleanup empties the consensus text -> empty entry, conf kept
// (core.py 670-671: if not text: return "", confidence).
// cleanup_subtitle_text strips attached latin from CJK; a pure-latin sample
// under a CJK profile with no CJK chars survives cleanup as-is, so we feed a
// sample that cleanup maps to empty: a single '.' becomes '…' (not empty),
// hence we use a whitespace-only line that normalize_ocr_text clears. The
// consensus path already filters empty normalized samples, so we instead
// exercise the post-cleanup empty branch via a sample whose cleanup result
// is empty. cleanup_subtitle_text returns {} only when normalize_ocr_text is
// empty; that path is reached when consensus_text picks a medoid whose
// normalized form is non-empty but cleanup strips it entirely. Constructing
// that deterministically requires a CJK sample that is all attached latin --
// not achievable with our line_result helper. This case is covered instead by
// the should_accept rejection path below (which returns ("", conf) for a
// non-empty but rejected text).
//
// We keep a smoke test for the consensus.text-empty branch (core.py 661-662):
// all samples normalize to empty -> consensus returns ("", 0.0) with 0 votes.
TEST_CASE("ocr line_select: all-empty samples -> consensus empty, 0 conf",
          "[pipeline][ocr]") {
  auto cfg = base_cfg();
  cfg.subtitle_script = "cjk";
  auto det = full_detector();
  // Lines with whitespace-only text: normalize_ocr_text clears them, so
  // samples stay empty -> consensus_text returns ("", 0.0, 0). The line_select
  // loop `continue`s on empty normalized text, samples stays empty, and the
  // post-loop consensus.text is empty -> return ("", 0.0).
  auto ocr = seq(line_result("   ", 0.9));
  sublift::Pipeline pipe(det, ocr, cfg);
  (void)pipe.feed(empty_frame(0));
  auto e = pipe.ocr_segment(
      make_event(100, 300, empty_frame(200), frames(empty_frame(400))));
  REQUIRE(e.text.empty());
  REQUIRE(e.confidence == 0.0);
  REQUIRE(ocr.call_count() == 2);  // both frames OCR'd, both filtered out
}

// line_select: should_accept rejects low-conf single-frame text but conf kept
// (core.py 681-682: if not accept: return "", confidence). The
// "low-confidence single frame rejected" case above uses default low_conf and
// a single anchor frame. Here we cover the same rejection branch with a
// fallback present: two frames OCR'd (no early stop because conf < low_conf),
// samples has 2 entries but the partial consensus early-stop requires
// conf >= low_conf_threshold, so the loop runs to exhaustion; the final
// consensus has support_votes=2 but confidence < low_conf_threshold, so
// should_accept rejects via all branches -> text blanked, conf kept.
TEST_CASE("ocr line_select: sub-low-conf consensus rejected, conf kept",
          "[pipeline][ocr]") {
  auto cfg = base_cfg();
  cfg.subtitle_script = "cjk";
  cfg.confidence_threshold = 0.7;
  cfg.low_conf_threshold = 0.5;  // consensus conf 0.2 < 0.5
  auto det = full_detector();
  // Both frames return the same CJK text at conf=0.2. No single_high_conf
  // early stop (0.2 < 0.7). No two_frame_consensus early stop (0.2 < 0.5).
  // Final consensus: votes=2, conf=0.2. should_accept: 0.2 < 0.7 (rule 1),
  // 0.2 < low_conf 0.5 (rule 2), 0.2 < max(0.5,0.35)=0.5 (rule 3),
  // 0.2 < 0.7 (rule 4) -> reject.
  auto ocr = seq(line_result("你好", 0.2));
  sublift::Pipeline pipe(det, ocr, cfg);
  (void)pipe.feed(empty_frame(0));
  auto e = pipe.ocr_segment(
      make_event(100, 300, empty_frame(200), frames(empty_frame(400))));
  REQUIRE(e.text.empty());
  REQUIRE(e.confidence == 0.2);  // consensus conf preserved
  REQUIRE(ocr.call_count() == 2);
}
