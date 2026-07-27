#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include "sublift/fixed_detector.hpp"
#include "sublift/mock_ocr.hpp"
#include "sublift/models.hpp"
#include "sublift/pipeline.hpp"
#include "sublift/test_support.hpp"

#include "../pipeline_test_helpers.hpp"

#ifndef SUBLIFT_PARITY_GOLDEN_PIPELINE
#error "SUBLIFT_PARITY_GOLDEN_PIPELINE must be defined by CMake"
#endif

using namespace sublift::test;
namespace tsv = sublift::test_support;

namespace {

// Rebuild a frame from the golden pattern name (mirrors Python make_frame).
sublift::Frame make_frame(std::int64_t ts, const std::string& pattern) {
  if (pattern == "empty") {
    return empty_frame(ts);
  }
  if (pattern == "subA") {
    return subA(ts);
  }
  if (pattern == "subB") {
    return subB(ts);
  }
  if (pattern == "subCyan") {
    return subCyan(ts);
  }
  throw std::runtime_error("unknown pattern: " + pattern);
}

// Build a MockOcrEngine from the golden mock setup (mirrors Python build_mock).
sublift::MockOcrEngine build_mock(const tsv::PipelineMockOcrGolden& setup) {
  std::vector<sublift::OcrResult> results;
  for (const auto& r : setup.results) {
    if (!r.lines.empty()) {
      std::vector<sublift::OcrLine> lines;
      for (const auto& l : r.lines) {
        lines.push_back(sublift::OcrLine{l.text, l.confidence, l.box});
      }
      results.push_back(sublift::OcrResult::from_lines(lines));
    } else {
      results.push_back(sublift::OcrResult{r.text, r.confidence, {}});
    }
  }
  if (setup.mode == "fixed") {
    const auto& first = results.at(0);
    return sublift::MockOcrEngine(first.text, first.confidence);
  }
  // sequence mode: pad with the last result to avoid out_of_range on over-call
  // (Python side pads the same way; expected_ocr_calls is computed at dump).
  if (!results.empty()) {
    while (results.size() < setup.results.size() + 8) {
      results.push_back(results.back());
    }
  }
  return sublift::MockOcrEngine(std::move(results));
}

}  // namespace

TEST_CASE("pipeline parity vs frozen golden", "[parity][pipeline]") {
  const auto golden_path = std::filesystem::path{SUBLIFT_PARITY_GOLDEN_PIPELINE};
  const auto golden = tsv::load_pipeline_golden(golden_path);

  REQUIRE(golden.oracle.golden_schema_version == 1);
  REQUIRE_FALSE(golden.oracle.oracle_commit.empty());
  REQUIRE_FALSE(golden.scenarios.empty());

  for (const auto& sc : golden.scenarios) {
    auto det = full_detector();
    auto mock = build_mock(sc.mock_ocr);
    sublift::Pipeline pipe(det, mock, sc.config);

    std::vector<tsv::PipelineSegmentEventGolden> cand_events;
    std::vector<tsv::PipelineEntryGolden> cand_raw;
    std::vector<tsv::PipelineEntryGolden> cand_final;
    bool finalized = false;

    for (const auto& fr : sc.frames) {
      if (fr.action == "cancel") {
        pipe.cancel();
        continue;
      }
      if (fr.action == "finalize") {
        auto out = pipe.finalize();
        for (const auto& e : out) {
          cand_final.push_back({e.start_ms, e.end_ms, e.text, e.confidence});
        }
        finalized = true;
        continue;
      }
      auto frame = make_frame(fr.ts, fr.pattern);
      auto ev = pipe.feed(frame);
      if (ev.has_value()) {
        tsv::PipelineSegmentEventGolden seg;
        seg.start_ms = ev->start_ms;
        seg.end_ms = ev->end_ms;
        if (ev->anchor_frame.has_value()) {
          seg.anchor_ts = ev->anchor_frame->timestamp_ms;
        }
        for (const auto& f : ev->fallback_frames) {
          seg.fallback_ts.push_back(f.timestamp_ms);
        }
        cand_events.push_back(seg);
        auto entry = pipe.ocr_segment(*ev);
        cand_raw.push_back({entry.start_ms, entry.end_ms, entry.text,
                            entry.confidence});
      }
    }

    if (!finalized) {
      auto out = pipe.finalize();
      for (const auto& e : out) {
        cand_final.push_back({e.start_ms, e.end_ms, e.text, e.confidence});
      }
    }

    const auto diff = tsv::diff_pipeline(
        cand_events, cand_raw, cand_final,
        static_cast<int>(mock.call_count()), sc, &golden.oracle);
    INFO("scenario: " << sc.name << "\n"
                      << (diff.has_value() ? *diff : std::string{"ok"}));
    REQUIRE_FALSE(diff.has_value());
  }
}
