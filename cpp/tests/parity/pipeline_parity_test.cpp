#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <golden_loader.hpp>
#include <sublift/adapters/mock/mock_frame_source.hpp>
#include <sublift/adapters/mock/mock_ocr.hpp>
#include <sublift/adapters/mock/mock_subtitle_sink.hpp>
#include <sublift/core/box.hpp>
#include <sublift/core/color.hpp>
#include <sublift/core/frame.hpp>
#include <sublift/core/image_buffer.hpp>
#include <sublift/core/models.hpp>
#include <sublift/core/subtitle.hpp>
#include <sublift/engine/config.hpp>
#include <sublift/engine/observer.hpp>
#include <sublift/engine/session.hpp>
#include <sublift/ports/detector.hpp>

#ifndef SUBLIFT_PROJECT_SOURCE_DIR
#error "SUBLIFT_PROJECT_SOURCE_DIR must be defined by CMake"
#endif

namespace {

// ============================================================================
// Synthetic Frame Generation & Fixed Detector
// ============================================================================

constexpr std::int32_t kW = 128;
constexpr std::int32_t kH = 64;
constexpr std::uint8_t kBg = 40;
constexpr std::uint8_t kFg = 240;

sublift::Frame make_frame(std::int64_t ts, const std::string& pattern,
                          std::int32_t w = kW, std::int32_t h = kH) {
  sublift::ImageBuffer img(w, h, sublift::PixelFormat::RGB24);
  for (std::int32_t y = 0; y < h; ++y) {
    std::uint8_t* row = img.row(y);
    for (std::int32_t x = 0; x < w; ++x) {
      row[x * 3 + 0] = kBg;
      row[x * 3 + 1] = kBg;
      row[x * 3 + 2] = kBg;
    }
  }

  auto stamp = [&](std::int32_t x0, std::int32_t x1, std::int32_t y0, std::int32_t y1,
                   std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    for (std::int32_t y = y0; y < y1 && y < h; ++y) {
      std::uint8_t* row = img.row(y);
      for (std::int32_t x = x0; x < x1 && x < w; ++x) {
        row[x * 3 + 0] = r;
        row[x * 3 + 1] = g;
        row[x * 3 + 2] = b;
      }
    }
  };

  if (pattern == "empty") {
    // blank dark background
  } else if (pattern == "subA") {
    stamp(48, 80, 44, 60, kFg, kFg, kFg);
  } else if (pattern == "subB") {
    stamp(40, 88, 46, 58, kFg, kFg, kFg);
  } else if (pattern == "subCyan") {
    stamp(48, 80, 44, 60, 0, 255, 255);
  } else if (pattern == "subCyanStroked") {
    // Realistic stroked cyan glyphs (RGB(0, 255, 255)) with 2px strokes and 3px interior gaps
    for (std::int32_t x = 44; x < 84; x += 5) {
      stamp(x, x + 2, 44, 60, 0, 255, 255);
    }
    stamp(44, 84, 44, 46, 0, 255, 255);
    stamp(44, 84, 51, 53, 0, 255, 255);
    stamp(44, 84, 58, 60, 0, 255, 255);
  } else if (pattern == "subWide") {
    stamp(100, 412, 44, 60, kFg, kFg, kFg);
  } else if (pattern == "subWideChange") {
    stamp(120, 392, 44, 60, kFg, kFg, kFg);
  } else {
    throw std::runtime_error("Unknown golden pattern: " + pattern);
  }

  return sublift::Frame{
      .timestamp_ms = ts,
      .image = std::move(img),
  };
}

class FixedRegionDetector final : public sublift::IDetector {
 public:
  explicit FixedRegionDetector(sublift::FrameLocalBox box = sublift::FrameLocalBox{0, 0, kW, kH})
      : box_(box) {}

  std::optional<sublift::Region> detect(const sublift::Frame&) override {
    return sublift::Region{box_};
  }

 private:
  sublift::FrameLocalBox box_;
};

// ============================================================================
// Golden Models & Structures
// ============================================================================

struct GoldenOcrLine {
  std::string text;
  double confidence{0.0};
  sublift::OcrCropBox box{};
};

struct GoldenOcrResult {
  std::string text;
  double confidence{0.0};
  std::vector<GoldenOcrLine> lines{};
};

struct GoldenMockOcrSetup {
  std::string mode{"sequence"};
  std::vector<GoldenOcrResult> results{};
};

struct GoldenFrame {
  std::int64_t ts{0};
  std::string pattern;
  std::string action; // "feed", "cancel", "finalize"
};

struct GoldenSegmentEvent {
  std::int64_t start_ms{0};
  std::int64_t end_ms{0};
  std::optional<std::int64_t> anchor_ts{std::nullopt};
  std::vector<std::int64_t> fallback_ts{};
};

struct GoldenEntry {
  std::int64_t start_ms{0};
  std::int64_t end_ms{0};
  std::string text{};
  double confidence{0.0};
};

struct GoldenScenario {
  std::string name;
  std::int32_t width{kW};
  std::int32_t height{kH};
  std::map<std::string, nlohmann::json> config_overrides{};
  GoldenMockOcrSetup mock_ocr{};
  std::vector<GoldenFrame> frames{};
  std::vector<GoldenSegmentEvent> expected_segment_events{};
  std::vector<GoldenEntry> expected_raw_entries{};
  int expected_ocr_calls{0};
  std::vector<GoldenEntry> expected_final_entries{};
};

struct GoldenFile {
  int golden_schema_version{0};
  std::string oracle_commit{};
  std::map<std::string, nlohmann::json> default_config_overrides{};
  std::vector<GoldenScenario> scenarios{};
};

GoldenFile load_golden_file(const std::filesystem::path& path) {
  auto root = sublift::test::load_golden_json(path);

  GoldenFile gf;
  gf.golden_schema_version = root["golden_schema_version"].get<int>();
  gf.oracle_commit = root["oracle"]["oracle_commit"].get<std::string>();
  if (root.contains("default_config_overrides")) {
    for (auto it = root["default_config_overrides"].begin(); it != root["default_config_overrides"].end(); ++it) {
      gf.default_config_overrides[it.key()] = it.value();
    }
  }

  for (const auto& sc_json : root["scenarios"]) {
    GoldenScenario sc;
    sc.name = sc_json["name"].get<std::string>();

    if (sc_json.contains("config_overrides")) {
      for (auto it = sc_json["config_overrides"].begin(); it != sc_json["config_overrides"].end(); ++it) {
        sc.config_overrides[it.key()] = it.value();
      }
    }

    const auto& mock_json = sc_json["mock_ocr_setup"];
    sc.mock_ocr.mode = mock_json["mode"].get<std::string>();
    for (const auto& res_json : mock_json["results"]) {
      GoldenOcrResult res;
      res.text = res_json["text"].get<std::string>();
      res.confidence = res_json["confidence"].get<double>();
      if (res_json.contains("lines")) {
        for (const auto& l_json : res_json["lines"]) {
          GoldenOcrLine line;
          line.text = l_json["text"].get<std::string>();
          line.confidence = l_json["confidence"].get<double>();
          const auto& box_j = l_json["box"];
          line.box = sublift::OcrCropBox{
              .x = box_j["x"].get<std::int32_t>(),
              .y = box_j["y"].get<std::int32_t>(),
              .width = box_j["width"].get<std::int32_t>(),
              .height = box_j["height"].get<std::int32_t>(),
          };
          res.lines.push_back(line);
        }
      }
      sc.mock_ocr.results.push_back(res);
    }

    for (const auto& fr_json : sc_json["frames"]) {
      GoldenFrame fr;
      fr.ts = fr_json["ts"].get<std::int64_t>();
      fr.pattern = fr_json["pattern"].get<std::string>();
      if (fr_json.contains("action")) {
        fr.action = fr_json["action"].get<std::string>();
      }
      sc.frames.push_back(fr);
    }

    for (const auto& ev_json : sc_json["expected_segment_events"]) {
      GoldenSegmentEvent ev;
      ev.start_ms = ev_json["start_ms"].get<std::int64_t>();
      ev.end_ms = ev_json["end_ms"].get<std::int64_t>();
      if (ev_json.contains("anchor_ts") && !ev_json["anchor_ts"].is_null()) {
        ev.anchor_ts = ev_json["anchor_ts"].get<std::int64_t>();
      }
      if (ev_json.contains("fallback_ts")) {
        for (const auto& fb : ev_json["fallback_ts"]) {
          ev.fallback_ts.push_back(fb.get<std::int64_t>());
        }
      }
      sc.expected_segment_events.push_back(ev);
    }

    for (const auto& re_json : sc_json["expected_raw_entries"]) {
      sc.expected_raw_entries.push_back(GoldenEntry{
          .start_ms = re_json["start_ms"].get<std::int64_t>(),
          .end_ms = re_json["end_ms"].get<std::int64_t>(),
          .text = re_json["text"].get<std::string>(),
          .confidence = re_json["confidence"].get<double>(),
      });
    }

    sc.expected_ocr_calls = sc_json["expected_ocr_calls"].get<int>();

    for (const auto& fe_json : sc_json["expected_final_entries"]) {
      sc.expected_final_entries.push_back(GoldenEntry{
          .start_ms = fe_json["start_ms"].get<std::int64_t>(),
          .end_ms = fe_json["end_ms"].get<std::int64_t>(),
          .text = fe_json["text"].get<std::string>(),
          .confidence = fe_json["confidence"].get<double>(),
      });
    }

    gf.scenarios.push_back(std::move(sc));
  }

  return gf;
}

// ============================================================================
// Recording Extraction Observer
// ============================================================================

class RecordingObserver final : public sublift::IExtractionObserver {
 public:
  void on_segment_detected(const sublift::timing::Segment& seg) override {
    std::lock_guard<std::mutex> lock(mutex_);
    GoldenSegmentEvent ev;
    ev.start_ms = seg.start_ms;
    ev.end_ms = seg.end_ms;
    ev.anchor_ts = seg.anchor_timestamp_ms;
    ev.fallback_ts = seg.fallback_timestamps_ms;
    segments_.push_back(ev);
  }

  void on_subtitle_entry(const sublift::SubtitleEntry& entry) override {
    std::lock_guard<std::mutex> lock(mutex_);
    raw_entries_.push_back(GoldenEntry{
        .start_ms = entry.range.start_ms,
        .end_ms = entry.range.end_ms,
        .text = entry.text,
        .confidence = entry.confidence,
    });
  }

  void on_progress(std::int64_t current_ms, std::int64_t total_ms, double fps) override {
    (void)current_ms;
    (void)total_ms;
    (void)fps;
  }

  void on_complete(const sublift::ExtractionMetrics& metrics) override {
    (void)metrics;
  }

  void on_error(const sublift::ExtractionError& error) override {
    (void)error;
  }

  std::vector<GoldenSegmentEvent> segments() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return segments_;
  }

  std::vector<GoldenEntry> raw_entries() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return raw_entries_;
  }

 private:
  mutable std::mutex mutex_;
  std::vector<GoldenSegmentEvent> segments_;
  std::vector<GoldenEntry> raw_entries_;
};

sublift::adapters::mock::MockOcrEngine build_mock_ocr(const GoldenMockOcrSetup& setup) {
  std::vector<sublift::OcrResult> results;
  for (const auto& r : setup.results) {
    if (!r.lines.empty()) {
      std::vector<sublift::OcrLine> lines;
      lines.reserve(r.lines.size());
      for (const auto& l : r.lines) {
        lines.push_back(sublift::OcrLine{
            .text = l.text,
            .confidence = l.confidence,
            .box = l.box,
        });
      }
      results.push_back(sublift::OcrResult::from_lines(lines));
    } else {
      results.push_back(sublift::OcrResult{
          .text = r.text,
          .confidence = r.confidence,
          .lines = {},
      });
    }
  }

  if (setup.mode == "fixed") {
    if (!results.empty()) {
      return sublift::adapters::mock::MockOcrEngine(results[0].text, results[0].confidence);
    }
    return sublift::adapters::mock::MockOcrEngine();
  }

  // sequence mode: pad with the last result to avoid out_of_range on over-call
  if (!results.empty()) {
    while (results.size() < setup.results.size() + 8) {
      results.push_back(results.back());
    }
  }
  return sublift::adapters::mock::MockOcrEngine(std::move(results));
}

sublift::ExtractionConfig build_config(
    const std::map<std::string, nlohmann::json>& default_overrides,
    const std::map<std::string, nlohmann::json>& sc_overrides,
    std::size_t num_threads) {
  sublift::ExtractionConfig cfg{};
  cfg.num_threads = num_threads;
  cfg.sample_fps = 5.0;
  // Python oracle dump_pipeline baseline was generated with default hysteresis_frames = 1 (0ms)
  cfg.hysteresis_ms = 0;

  auto apply_override = [&](const std::string& k, const nlohmann::json& v) {
    if (k == "change_point.change_threshold") {
      cfg.change_threshold = v.get<std::int32_t>();
    } else if (k == "confidence_threshold") {
      cfg.confidence_threshold = v.get<double>();
    } else if (k == "low_conf_threshold") {
      cfg.low_conf_threshold = v.get<double>();
    } else if (k == "ocr_consensus_frames") {
      cfg.ocr_consensus_frames = v.get<std::int32_t>();
    } else if (k == "min_duration_ms") {
      cfg.min_duration_ms = v.get<std::int64_t>();
    } else if (k == "merge_gap_ms") {
      cfg.merge_gap_ms = v.get<std::int64_t>();
    } else if (k == "drop_empty_text") {
      cfg.drop_empty_text = v.get<bool>();
    } else if (k == "subtitle_script") {
      cfg.subtitle_script = v.get<std::string>();
    } else if (k == "change_point.enable_ssim_patrol") {
      cfg.enable_ssim_patrol = v.get<bool>();
    } else if (k == "enable_line_select") {
      cfg.enable_line_select = v.get<bool>();
    } else if (k == "ocr_anchor_delay_ms") {
      cfg.ocr_anchor_delay_ms = v.get<std::int64_t>();
    } else if (k == "ocr_anchor_delay_frames") {
      cfg.ocr_anchor_delay_ms = static_cast<std::int64_t>(v.get<int>()) * 200;
    } else if (k == "change_point.hysteresis_ms") {
      cfg.hysteresis_ms = v.get<std::int64_t>();
    } else if (k == "change_point.hysteresis_frames") {
      const int hf = v.get<int>();
      cfg.hysteresis_ms = (hf <= 1) ? 0 : static_cast<std::int64_t>(hf - 1) * 200;
    } else if (k == "change_point.patrol_interval_ms") {
      cfg.patrol_interval_ms = v.get<std::int64_t>();
    } else if (k == "change_point.ssim_patrol_interval") {
      cfg.patrol_interval_ms = static_cast<std::int64_t>(v.get<int>()) * 200;
    }
  };

  for (const auto& [k, v] : default_overrides) {
    apply_override(k, v);
  }
  for (const auto& [k, v] : sc_overrides) {
    apply_override(k, v);
  }

  return cfg;
}

struct RunOutput {
  std::vector<GoldenSegmentEvent> segment_events;
  std::vector<GoldenEntry> raw_entries;
  std::vector<GoldenEntry> final_entries;
  std::size_t ocr_calls{0};
};

RunOutput run_scenario(
    const GoldenScenario& sc,
    const std::map<std::string, nlohmann::json>& default_overrides,
    std::size_t num_threads) {
  sublift::adapters::mock::MockFrameSource source;
  sublift::adapters::mock::MockOcrEngine ocr = build_mock_ocr(sc.mock_ocr);
  FixedRegionDetector detector(sublift::FrameLocalBox{0, 0, sc.width, sc.height});
  sublift::adapters::mock::MockSubtitleSink sink;

  auto cfg = build_config(default_overrides, sc.config_overrides, num_threads);

  sublift::ExtractionSession session(source, ocr, &detector, &sink, cfg);
  RecordingObserver observer;
  session.set_observer(&observer);

  bool finalized = false;
  for (const auto& fr : sc.frames) {
    if (fr.action == "cancel") {
      session.cancel();
      continue;
    }
    if (fr.action == "finalize") {
      auto out = session.finalize();
      (void)out;
      finalized = true;
      continue;
    }
    session.feed(make_frame(fr.ts, fr.pattern, sc.width, sc.height));
  }

  if (!finalized) {
    session.finalize();
  }

  RunOutput out;
  out.segment_events = observer.segments();
  out.raw_entries = observer.raw_entries();
  out.ocr_calls = ocr.call_count();

  for (const auto& entry : sink.entries()) {
    out.final_entries.push_back(GoldenEntry{
        .start_ms = entry.range.start_ms,
        .end_ms = entry.range.end_ms,
        .text = entry.text,
        .confidence = entry.confidence,
    });
  }

  return out;
}

std::string format_diff(const RunOutput& actual, const GoldenScenario& exp) {
  std::ostringstream ss;

  // Segment Events
  if (actual.segment_events.size() != exp.expected_segment_events.size()) {
    ss << "  [SegmentEvents Count]: actual=" << actual.segment_events.size()
       << " expected=" << exp.expected_segment_events.size() << "\n";
  }
  std::size_t seg_n = std::min(actual.segment_events.size(), exp.expected_segment_events.size());
  for (std::size_t i = 0; i < seg_n; ++i) {
    const auto& a = actual.segment_events[i];
    const auto& e = exp.expected_segment_events[i];
    if (a.start_ms != e.start_ms || a.end_ms != e.end_ms || a.anchor_ts != e.anchor_ts ||
        a.fallback_ts != e.fallback_ts) {
      ss << "  [SegmentEvent #" << i << " mismatch]:\n"
         << "    expected: [" << e.start_ms << "," << e.end_ms << "] anchor="
         << (e.anchor_ts ? std::to_string(*e.anchor_ts) : "null") << " fallback=[";
      for (std::size_t k = 0; k < e.fallback_ts.size(); ++k) {
        if (k > 0) ss << ",";
        ss << e.fallback_ts[k];
      }
      ss << "]\n    actual:   [" << a.start_ms << "," << a.end_ms << "] anchor="
         << (a.anchor_ts ? std::to_string(*a.anchor_ts) : "null") << " fallback=[";
      for (std::size_t k = 0; k < a.fallback_ts.size(); ++k) {
        if (k > 0) ss << ",";
        ss << a.fallback_ts[k];
      }
      ss << "]\n";
    }
  }

  // OCR Calls
  if (actual.ocr_calls != static_cast<std::size_t>(exp.expected_ocr_calls)) {
    ss << "  [OCR Calls]: actual=" << actual.ocr_calls
       << " expected=" << exp.expected_ocr_calls << "\n";
  }

  // Raw Entries
  if (actual.raw_entries.size() != exp.expected_raw_entries.size()) {
    ss << "  [RawEntries Count]: actual=" << actual.raw_entries.size()
       << " expected=" << exp.expected_raw_entries.size() << "\n";
  }
  std::size_t raw_n = std::min(actual.raw_entries.size(), exp.expected_raw_entries.size());
  for (std::size_t i = 0; i < raw_n; ++i) {
    const auto& a = actual.raw_entries[i];
    const auto& e = exp.expected_raw_entries[i];
    if (a.start_ms != e.start_ms || a.end_ms != e.end_ms || a.text != e.text) {
      ss << "  [RawEntry #" << i << " mismatch]:\n"
         << "    expected: [" << e.start_ms << "," << e.end_ms << "] \"" << e.text << "\"\n"
         << "    actual:   [" << a.start_ms << "," << a.end_ms << "] \"" << a.text << "\"\n";
    }
  }

  // Final Entries
  if (actual.final_entries.size() != exp.expected_final_entries.size()) {
    ss << "  [FinalEntries Count]: actual=" << actual.final_entries.size()
       << " expected=" << exp.expected_final_entries.size() << "\n";
  }
  std::size_t fin_n = std::min(actual.final_entries.size(), exp.expected_final_entries.size());
  for (std::size_t i = 0; i < fin_n; ++i) {
    const auto& a = actual.final_entries[i];
    const auto& e = exp.expected_final_entries[i];
    if (a.start_ms != e.start_ms || a.end_ms != e.end_ms || a.text != e.text) {
      ss << "  [FinalEntry #" << i << " mismatch]:\n"
         << "    expected: [" << e.start_ms << "," << e.end_ms << "] \"" << e.text << "\"\n"
         << "    actual:   [" << a.start_ms << "," << a.end_ms << "] \"" << a.text << "\"\n";
    }
  }

  return ss.str();
}

} // namespace

// ============================================================================
// Pipeline Golden Parity Test Suite
// ============================================================================

TEST_CASE("Pipeline golden parity test across threads (pipeline.v1.json)", "[parity][pipeline][e2e]") {
  const auto project_root = std::filesystem::path{SUBLIFT_PROJECT_SOURCE_DIR};
  const auto golden_path = project_root / "benchmark/parity/goldens/pipeline/pipeline.v1.json";

  INFO("Loading golden from: " << golden_path.string());
  GoldenFile golden = load_golden_file(golden_path);

  REQUIRE(golden.golden_schema_version == 1);
  REQUIRE_FALSE(golden.oracle_commit.empty());
  REQUIRE(golden.scenarios.size() == 8);

  for (const auto& sc : golden.scenarios) {
    DYNAMIC_SECTION("Scenario: " << sc.name) {
      // 1. Single-threaded execution (baseline)
      RunOutput single_run = run_scenario(sc, golden.default_config_overrides, /*num_threads=*/1);
      std::string diff_single = format_diff(single_run, sc);
      INFO("Single-threaded diff vs golden:\n" << diff_single);
      REQUIRE(diff_single.empty());

      // 2. Multi-threaded execution (num_threads = 2)
      RunOutput mt2_run = run_scenario(sc, golden.default_config_overrides, /*num_threads=*/2);
      std::string diff_mt2 = format_diff(mt2_run, sc);
      INFO("Multi-threaded (2T) diff vs golden:\n" << diff_mt2);
      REQUIRE(diff_mt2.empty());

      // 3. Multi-threaded execution (num_threads = 4)
      RunOutput mt4_run = run_scenario(sc, golden.default_config_overrides, /*num_threads=*/4);
      std::string diff_mt4 = format_diff(mt4_run, sc);
      INFO("Multi-threaded (4T) diff vs golden:\n" << diff_mt4);
      REQUIRE(diff_mt4.empty());

      // 4. Strict cross-thread deterministic equivalence
      REQUIRE(single_run.final_entries.size() == mt2_run.final_entries.size());
      REQUIRE(single_run.final_entries.size() == mt4_run.final_entries.size());
      for (std::size_t i = 0; i < single_run.final_entries.size(); ++i) {
        REQUIRE(single_run.final_entries[i].start_ms == mt2_run.final_entries[i].start_ms);
        REQUIRE(single_run.final_entries[i].end_ms == mt2_run.final_entries[i].end_ms);
        REQUIRE(single_run.final_entries[i].text == mt2_run.final_entries[i].text);

        REQUIRE(single_run.final_entries[i].start_ms == mt4_run.final_entries[i].start_ms);
        REQUIRE(single_run.final_entries[i].end_ms == mt4_run.final_entries[i].end_ms);
        REQUIRE(single_run.final_entries[i].text == mt4_run.final_entries[i].text);
      }
    }
  }
}

TEST_CASE("Pipeline wide-frame (aspect > 6.0) with SSIM patrol golden parity across threads",
          "[parity][pipeline][wide]") {
  // Crop dimensions: 512x64 (aspect ratio = 8.0 > 6.0 threshold)
  constexpr std::int32_t kWide = 512;
  constexpr std::int32_t kHWide = 64;

  GoldenScenario sc;
  sc.name = "wide_frame_ssim_patrol";
  sc.width = kWide;
  sc.height = kHWide;
  sc.config_overrides["change_point.enable_ssim_patrol"] = true;
  sc.config_overrides["change_point.patrol_interval_ms"] = 600;
  sc.config_overrides["ocr_anchor_delay_ms"] = 400;
  sc.config_overrides["enable_line_select"] = false;
  sc.config_overrides["confidence_threshold"] = 0.0;
  sc.config_overrides["min_duration_ms"] = 0;

  sc.mock_ocr.mode = "fixed";
  sc.mock_ocr.results.push_back(GoldenOcrResult{
      .text = "Wide Subtitle Text",
      .confidence = 0.95,
      .lines = {},
  });

  // Frames at 5 FPS (200ms interval):
  // 0ms, 200ms: empty
  // 400ms: subWide (IN event confirmed at 600ms, start=400ms)
  // 600ms, 800ms, 1000ms, 1200ms, 1400ms, 1600ms: subWide (stable frames, SSIM patrol triggers at 1000ms and 1600ms)
  // 1800ms, 2000ms: empty (OUT event confirmed at 2000ms, end=1800ms)
  sc.frames = {
      GoldenFrame{0, "empty", "feed"},
      GoldenFrame{200, "empty", "feed"},
      GoldenFrame{400, "subWide", "feed"},
      GoldenFrame{600, "subWide", "feed"},
      GoldenFrame{800, "subWide", "feed"},
      GoldenFrame{1000, "subWide", "feed"},
      GoldenFrame{1200, "subWide", "feed"},
      GoldenFrame{1400, "subWide", "feed"},
      GoldenFrame{1600, "subWide", "feed"},
      GoldenFrame{1800, "empty", "feed"},
      GoldenFrame{2000, "empty", "feed"},
      GoldenFrame{2200, "empty", "finalize"},
  };

  sc.expected_segment_events = {
      GoldenSegmentEvent{
          .start_ms = 400,
          .end_ms = 1800,
          .anchor_ts = 800, // Locked at 400 + 400 = 800ms
          .fallback_ts = {600, 1000, 1600, 1800},
      },
  };
  sc.expected_ocr_calls = 1;
  sc.expected_raw_entries = {
      GoldenEntry{400, 1800, "Wide Subtitle Text", 0.95},
  };
  sc.expected_final_entries = {
      GoldenEntry{400, 1800, "Wide Subtitle Text", 0.95},
  };

  // Run across 1T, 2T, 4T and verify deterministic parity
  RunOutput run1 = run_scenario(sc, {}, 1);
  std::string diff1 = format_diff(run1, sc);
  INFO("Wide-frame 1T diff:\n" << diff1);
  REQUIRE(diff1.empty());

  RunOutput run2 = run_scenario(sc, {}, 2);
  std::string diff2 = format_diff(run2, sc);
  INFO("Wide-frame 2T diff:\n" << diff2);
  REQUIRE(diff2.empty());

  RunOutput run4 = run_scenario(sc, {}, 4);
  std::string diff4 = format_diff(run4, sc);
  INFO("Wide-frame 4T diff:\n" << diff4);
  REQUIRE(diff4.empty());

  REQUIRE(run1.final_entries.size() == run2.final_entries.size());
  REQUIRE(run1.final_entries.size() == run4.final_entries.size());
  for (std::size_t i = 0; i < run1.final_entries.size(); ++i) {
    REQUIRE(run1.final_entries[i].start_ms == run2.final_entries[i].start_ms);
    REQUIRE(run1.final_entries[i].end_ms == run2.final_entries[i].end_ms);
    REQUIRE(run1.final_entries[i].text == run2.final_entries[i].text);

    REQUIRE(run1.final_entries[i].start_ms == run4.final_entries[i].start_ms);
    REQUIRE(run1.final_entries[i].end_ms == run4.final_entries[i].end_ms);
    REQUIRE(run1.final_entries[i].text == run4.final_entries[i].text);
  }
}

TEST_CASE("Stroked cyan subtitle glyphs survive RGB2GRAY adaptive mask (DEV-006)",
          "[parity][pipeline][cyan]") {
  GoldenScenario sc;
  sc.name = "stroked_cyan_subtitle_detection";
  sc.width = kW;
  sc.height = kH;
  sc.config_overrides["enable_line_select"] = false;
  sc.config_overrides["confidence_threshold"] = 0.0;
  sc.config_overrides["min_duration_ms"] = 0;

  sc.mock_ocr.mode = "fixed";
  sc.mock_ocr.results.push_back(GoldenOcrResult{
      .text = "青字",
      .confidence = 0.94,
      .lines = {},
  });

  sc.frames = {
      GoldenFrame{0, "empty", "feed"},
      GoldenFrame{100, "subCyanStroked", "feed"},
      GoldenFrame{200, "subCyanStroked", "feed"},
      GoldenFrame{300, "subCyanStroked", "feed"},
      GoldenFrame{400, "subCyanStroked", "feed"},
      GoldenFrame{500, "empty", "feed"},
      GoldenFrame{600, "empty", "feed"},
      GoldenFrame{700, "empty", "finalize"},
  };

  sc.expected_segment_events = {
      GoldenSegmentEvent{
          .start_ms = 100,
          .end_ms = 500,
          .anchor_ts = 250,
          .fallback_ts = {200, 300, 400, 500},
      },
  };
  sc.expected_ocr_calls = 1;
  sc.expected_raw_entries = {
      GoldenEntry{100, 500, "青字", 0.94},
  };
  sc.expected_final_entries = {
      GoldenEntry{100, 500, "青字", 0.94},
  };

  RunOutput run1 = run_scenario(sc, {}, 1);
  REQUIRE(run1.final_entries.size() == 1);
  CHECK(run1.final_entries[0].start_ms == 100);
  CHECK(run1.final_entries[0].end_ms == 500);
  CHECK(run1.final_entries[0].text == "青字");
}

