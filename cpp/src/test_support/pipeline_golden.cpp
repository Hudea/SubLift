#include "sublift/test_support.hpp"

#include <algorithm>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <vector>

#include <nlohmann/json.hpp>

namespace sublift::test_support {
namespace {

using nlohmann::json;

[[nodiscard]] std::int32_t require_i32(const json& obj, const std::string& key) {
  if (!obj.contains(key)) {
    throw std::runtime_error("missing int field: " + key);
  }
  const auto& v = obj.at(key);
  if (!v.is_number_integer() && !v.is_number_unsigned()) {
    throw std::runtime_error("field not integer: " + key);
  }
  const auto wide = v.get<std::int64_t>();
  if (wide < std::numeric_limits<std::int32_t>::min() ||
      wide > std::numeric_limits<std::int32_t>::max()) {
    throw std::runtime_error("int field out of int32 range: " + key);
  }
  return static_cast<std::int32_t>(wide);
}

[[nodiscard]] std::int64_t require_i64(const json& obj, const std::string& key) {
  if (!obj.contains(key)) {
    throw std::runtime_error("missing int64 field: " + key);
  }
  const auto& v = obj.at(key);
  if (!v.is_number_integer() && !v.is_number_unsigned()) {
    throw std::runtime_error("field not integer: " + key);
  }
  return v.get<std::int64_t>();
}

[[nodiscard]] double require_f64(const json& obj, const std::string& key) {
  if (!obj.contains(key)) {
    throw std::runtime_error("missing float field: " + key);
  }
  const auto& v = obj.at(key);
  if (!v.is_number()) {
    throw std::runtime_error("field not number: " + key);
  }
  return v.get<double>();
}

[[nodiscard]] std::string require_string(const json& obj, const std::string& key) {
  if (!obj.contains(key)) {
    throw std::runtime_error("missing string field: " + key);
  }
  const auto& v = obj.at(key);
  if (!v.is_string()) {
    throw std::runtime_error("field not string: " + key);
  }
  return v.get<std::string>();
}

[[nodiscard]] bool require_bool(const json& obj, const std::string& key) {
  if (!obj.contains(key)) {
    throw std::runtime_error("missing bool field: " + key);
  }
  const auto& v = obj.at(key);
  if (!v.is_boolean()) {
    throw std::runtime_error("field not bool: " + key);
  }
  return v.get<bool>();
}

void apply_override(Config& cfg, const std::string& path, const json& v) {
  // Whitelist of supported override paths (mirrors dump_pipeline.py).
  if (path == "change_point.enable_ssim_patrol") {
    cfg.change_point.enable_ssim_patrol = v.get<bool>();
  } else if (path == "change_point.change_threshold") {
    cfg.change_point.change_threshold = v.get<std::int32_t>();
  } else if (path == "change_point.hysteresis_frames") {
    cfg.change_point.hysteresis_frames = v.get<std::int32_t>();
  } else if (path == "enable_line_select") {
    cfg.enable_line_select = v.get<bool>();
  } else if (path == "confidence_threshold") {
    cfg.confidence_threshold = v.get<double>();
  } else if (path == "low_conf_threshold") {
    cfg.low_conf_threshold = v.get<double>();
  } else if (path == "ocr_anchor_delay_frames") {
    cfg.ocr_anchor_delay_frames = v.get<std::int32_t>();
  } else if (path == "ocr_consensus_frames") {
    cfg.ocr_consensus_frames = v.get<std::int32_t>();
  } else if (path == "min_duration_ms") {
    cfg.min_duration_ms = v.get<std::int32_t>();
  } else if (path == "merge_gap_ms") {
    cfg.merge_gap_ms = v.get<std::int32_t>();
  } else if (path == "drop_empty_text") {
    cfg.drop_empty_text = v.get<bool>();
  } else if (path == "subtitle_script") {
    cfg.subtitle_script = v.get<std::string>();
  } else {
    throw std::runtime_error("unknown config override path: " + path);
  }
}

Config parse_config(const json& root, const json& sc) {
  Config cfg{};
  if (root.contains("default_config_overrides") &&
      root.at("default_config_overrides").is_object()) {
    for (const auto& [k, v] : root.at("default_config_overrides").items()) {
      apply_override(cfg, k, v);
    }
  }
  if (sc.contains("config_overrides") && sc.at("config_overrides").is_object()) {
    for (const auto& [k, v] : sc.at("config_overrides").items()) {
      apply_override(cfg, k, v);
    }
  }
  return cfg;
}

OcrCropBox parse_ocr_box(const json& b) {
  return OcrCropBox{require_i32(b, "x"), require_i32(b, "y"),
                    require_i32(b, "width"), require_i32(b, "height")};
}

PipelineMockOcrGolden::Line parse_mock_line(const json& l) {
  PipelineMockOcrGolden::Line out;
  out.text = require_string(l, "text");
  out.confidence = require_f64(l, "confidence");
  out.box = parse_ocr_box(l.at("box"));
  return out;
}

PipelineMockOcrGolden::Result parse_mock_result(const json& r) {
  PipelineMockOcrGolden::Result out;
  out.text = require_string(r, "text");
  out.confidence = require_f64(r, "confidence");
  if (r.contains("lines") && r.at("lines").is_array()) {
    for (const auto& l : r.at("lines")) {
      out.lines.push_back(parse_mock_line(l));
    }
  }
  return out;
}

PipelineMockOcrGolden parse_mock(const json& m) {
  PipelineMockOcrGolden out;
  out.mode = require_string(m, "mode");
  if (!m.contains("results") || !m.at("results").is_array()) {
    throw std::runtime_error("mock_ocr_setup.results must be array");
  }
  for (const auto& r : m.at("results")) {
    out.results.push_back(parse_mock_result(r));
  }
  return out;
}

PipelineFrameGolden parse_frame(const json& f) {
  PipelineFrameGolden fg;
  fg.ts = require_i64(f, "ts");
  fg.pattern = require_string(f, "pattern");
  if (f.contains("action")) {
    fg.action = require_string(f, "action");
  }
  return fg;
}

PipelineSegmentEventGolden parse_seg_event(const json& e) {
  PipelineSegmentEventGolden eg;
  eg.start_ms = require_i64(e, "start_ms");
  eg.end_ms = require_i64(e, "end_ms");
  const auto& a = e.at("anchor_ts");
  if (a.is_null()) {
    eg.anchor_ts = std::nullopt;
  } else if (a.is_number_integer() || a.is_number_unsigned()) {
    eg.anchor_ts = a.get<std::int64_t>();
  } else {
    throw std::runtime_error("anchor_ts must be int or null");
  }
  if (!e.contains("fallback_ts") || !e.at("fallback_ts").is_array()) {
    throw std::runtime_error("fallback_ts must be array");
  }
  for (const auto& t : e.at("fallback_ts")) {
    eg.fallback_ts.push_back(t.get<std::int64_t>());
  }
  return eg;
}

PipelineEntryGolden parse_entry(const json& e) {
  return PipelineEntryGolden{
      require_i64(e, "start_ms"),
      require_i64(e, "end_ms"),
      require_string(e, "text"),
      require_f64(e, "confidence"),
  };
}

PipelineScenarioGolden parse_scenario(const json& sc, const json& root) {
  PipelineScenarioGolden s;
  s.name = require_string(sc, "name");
  s.config = parse_config(root, sc);
  s.detector_kind = require_string(sc.at("detector"), "kind");
  s.mock_ocr = parse_mock(sc.at("mock_ocr_setup"));
  if (!sc.contains("frames") || !sc.at("frames").is_array()) {
    throw std::runtime_error("scenario frames must be array");
  }
  for (const auto& f : sc.at("frames")) {
    s.frames.push_back(parse_frame(f));
  }
  if (!sc.contains("expected_segment_events") ||
      !sc.at("expected_segment_events").is_array()) {
    throw std::runtime_error("expected_segment_events must be array");
  }
  for (const auto& e : sc.at("expected_segment_events")) {
    s.expected_segment_events.push_back(parse_seg_event(e));
  }
  if (!sc.contains("expected_raw_entries") ||
      !sc.at("expected_raw_entries").is_array()) {
    throw std::runtime_error("expected_raw_entries must be array");
  }
  for (const auto& e : sc.at("expected_raw_entries")) {
    s.expected_raw_entries.push_back(parse_entry(e));
  }
  s.expected_ocr_calls = require_i32(sc, "expected_ocr_calls");
  if (!sc.contains("expected_final_entries") ||
      !sc.at("expected_final_entries").is_array()) {
    throw std::runtime_error("expected_final_entries must be array");
  }
  for (const auto& e : sc.at("expected_final_entries")) {
    s.expected_final_entries.push_back(parse_entry(e));
  }
  return s;
}

void diff_entries(std::ostringstream& body,
                  const std::vector<PipelineEntryGolden>& cand,
                  const std::vector<PipelineEntryGolden>& exp,
                  const char* label) {
  if (cand.size() != exp.size()) {
    body << "  " << label << " count: golden=" << exp.size()
         << " candidate=" << cand.size() << "\n";
  }
  const std::size_t n = std::min(cand.size(), exp.size());
  for (std::size_t i = 0; i < n; ++i) {
    const auto& c = cand[i];
    const auto& e = exp[i];
    if (c.start_ms != e.start_ms || c.end_ms != e.end_ms ||
        utf8_nfc(c.text) != utf8_nfc(e.text) ||
        c.confidence != e.confidence) {
      body << "  " << label << "[" << i << "]:\n"
           << "    golden: [" << e.start_ms << "," << e.end_ms << "] \""
           << e.text << "\" conf=" << e.confidence << "\n"
           << "    candidate: [" << c.start_ms << "," << c.end_ms << "] \""
           << c.text << "\" conf=" << c.confidence << "\n";
    }
  }
}

}  // namespace

PipelineGolden load_pipeline_golden(const std::filesystem::path& path) {
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("cannot open golden: " + path.string());
  }
  json root;
  in >> root;
  if (!root.is_object()) {
    throw std::runtime_error("golden root must be object");
  }
  if (!root.contains("golden_schema_version") ||
      !root.at("golden_schema_version").is_number_integer() ||
      root.at("golden_schema_version").get<int>() != 1) {
    throw std::runtime_error("golden_schema_version must be 1");
  }
  if (!root.contains("kind") || root.at("kind") != "pipeline") {
    throw std::runtime_error("kind must be pipeline");
  }
  if (!root.contains("oracle") || !root.at("oracle").is_object()) {
    throw std::runtime_error("missing oracle object");
  }
  if (!root.contains("scenarios") || !root.at("scenarios").is_array()) {
    throw std::runtime_error("missing scenarios array");
  }

  const auto& oj = root.at("oracle");
  OracleMeta oracle;
  oracle.golden_schema_version = 1;
  oracle.oracle_commit = require_string(oj, "oracle_commit");
  if (oracle.oracle_commit.empty()) {
    throw std::runtime_error("oracle_commit empty");
  }
  if (oj.contains("oracle_branch") && oj.at("oracle_branch").is_string()) {
    oracle.oracle_branch = oj.at("oracle_branch").get<std::string>();
  }
  if (oj.contains("config_fingerprint") && oj.at("config_fingerprint").is_string()) {
    oracle.config_fingerprint = oj.at("config_fingerprint").get<std::string>();
  }

  PipelineGolden out;
  out.oracle = std::move(oracle);
  for (const auto& s : root.at("scenarios")) {
    out.scenarios.push_back(parse_scenario(s, root));
  }
  return out;
}

std::optional<std::string> diff_pipeline(
    const std::vector<PipelineSegmentEventGolden>& cand_events,
    const std::vector<PipelineEntryGolden>& cand_raw,
    const std::vector<PipelineEntryGolden>& cand_final,
    int cand_ocr_calls,
    const PipelineScenarioGolden& exp,
    const OracleMeta* oracle) {
  std::ostringstream body;

  // 1. segment events
  if (cand_events.size() != exp.expected_segment_events.size()) {
    body << "  segment_events count: golden=" << exp.expected_segment_events.size()
         << " candidate=" << cand_events.size() << "\n";
  }
  {
    const std::size_t n = std::min(cand_events.size(), exp.expected_segment_events.size());
    for (std::size_t i = 0; i < n; ++i) {
      const auto& c = cand_events[i];
      const auto& e = exp.expected_segment_events[i];
      if (c.start_ms != e.start_ms || c.end_ms != e.end_ms ||
          c.anchor_ts != e.anchor_ts || c.fallback_ts != e.fallback_ts) {
        body << "  segment_event[" << i << "]:\n"
             << "    golden: [" << e.start_ms << "," << e.end_ms << "] anchor=";
        if (e.anchor_ts) {
          body << *e.anchor_ts;
        } else {
          body << "null";
        }
        body << " fallback_ts=[";
        for (std::size_t k = 0; k < e.fallback_ts.size(); ++k) {
          if (k) body << ",";
          body << e.fallback_ts[k];
        }
        body << "]\n    candidate: [" << c.start_ms << "," << c.end_ms << "] anchor=";
        if (c.anchor_ts) {
          body << *c.anchor_ts;
        } else {
          body << "null";
        }
        body << " fallback_ts=[";
        for (std::size_t k = 0; k < c.fallback_ts.size(); ++k) {
          if (k) body << ",";
          body << c.fallback_ts[k];
        }
        body << "]\n";
      }
    }
  }

  // 2. raw entries
  diff_entries(body, cand_raw, exp.expected_raw_entries, "raw_entry");

  // 3. final entries
  diff_entries(body, cand_final, exp.expected_final_entries, "final_entry");

  // 4. ocr calls
  if (cand_ocr_calls != exp.expected_ocr_calls) {
    body << "  ocr_calls: golden=" << exp.expected_ocr_calls
         << " candidate=" << cand_ocr_calls << "\n";
  }

  const auto detail = body.str();
  if (detail.empty()) {
    return std::nullopt;
  }
  std::ostringstream msg;
  msg << "pipeline parity mismatch scenario=" << exp.name << "\n";
  if (oracle != nullptr) {
    msg << "  oracle_commit: " << oracle->oracle_commit << "\n"
        << "  golden_schema_version: " << oracle->golden_schema_version << "\n";
  }
  msg << detail;
  return msg.str();
}

}  // namespace sublift::test_support
