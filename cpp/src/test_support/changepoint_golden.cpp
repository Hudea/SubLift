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

[[nodiscard]] std::int32_t require_i32(const json& obj, const char* key) {
  if (!obj.contains(key)) {
    throw std::runtime_error(std::string("missing int field: ") + key);
  }
  const auto& v = obj.at(key);
  if (!v.is_number_integer() && !v.is_number_unsigned()) {
    throw std::runtime_error(std::string("field not integer: ") + key);
  }
  const auto wide = v.get<std::int64_t>();
  if (wide < std::numeric_limits<std::int32_t>::min() ||
      wide > std::numeric_limits<std::int32_t>::max()) {
    throw std::runtime_error(std::string("int field out of int32 range: ") + key);
  }
  return static_cast<std::int32_t>(wide);
}

[[nodiscard]] std::int64_t require_i64(const json& obj, const char* key) {
  if (!obj.contains(key)) {
    throw std::runtime_error(std::string("missing int64 field: ") + key);
  }
  const auto& v = obj.at(key);
  if (!v.is_number_integer() && !v.is_number_unsigned()) {
    throw std::runtime_error(std::string("field not integer: ") + key);
  }
  return v.get<std::int64_t>();
}

[[nodiscard]] std::uint64_t require_u64(const json& obj, const char* key) {
  if (!obj.contains(key)) {
    throw std::runtime_error(std::string("missing uint field: ") + key);
  }
  const auto& v = obj.at(key);
  if (!v.is_number_integer() && !v.is_number_unsigned()) {
    throw std::runtime_error(std::string("field not integer: ") + key);
  }
  if (v.is_number_integer() && v.get<std::int64_t>() < 0) {
    throw std::runtime_error(std::string("uint field is negative: ") + key);
  }
  return v.get<std::uint64_t>();
}

[[nodiscard]] double require_f64(const json& obj, const char* key) {
  if (!obj.contains(key)) {
    throw std::runtime_error(std::string("missing float field: ") + key);
  }
  const auto& v = obj.at(key);
  if (!v.is_number()) {
    throw std::runtime_error(std::string("field not number: ") + key);
  }
  return v.get<double>();
}

[[nodiscard]] std::string require_string(const json& obj, const char* key) {
  if (!obj.contains(key)) {
    throw std::runtime_error(std::string("missing string field: ") + key);
  }
  const auto& v = obj.at(key);
  if (!v.is_string()) {
    throw std::runtime_error(std::string("field not string: ") + key);
  }
  return v.get<std::string>();
}

[[nodiscard]] bool require_bool(const json& obj, const char* key) {
  if (!obj.contains(key)) {
    throw std::runtime_error(std::string("missing bool field: ") + key);
  }
  const auto& v = obj.at(key);
  if (!v.is_boolean()) {
    throw std::runtime_error(std::string("field not bool: ") + key);
  }
  return v.get<bool>();
}

void require_only_keys(const json& obj, const std::vector<std::string>& allowed,
                       const char* context) {
  if (!obj.is_object()) {
    throw std::runtime_error(std::string(context) + " must be a JSON object");
  }
  for (const auto& el : obj.items()) {
    if (std::find(allowed.begin(), allowed.end(), el.key()) == allowed.end()) {
      throw std::runtime_error(std::string(context) + ": unknown key: " + el.key());
    }
  }
  for (const auto& key : allowed) {
    if (!obj.contains(key)) {
      throw std::runtime_error(std::string(context) + ": missing key: " + key);
    }
  }
}

ChangePointConfig parse_change_point(const json& obj) {
  require_only_keys(obj,
                    {"presence_threshold", "hysteresis_frames", "change_threshold",
                     "enable_ssim_verify", "ssim_threshold", "ssim_window_size",
                     "enable_ssim_patrol", "ssim_patrol_interval",
                     "ssim_patrol_threshold", "ssim_patrol_use_mask"},
                    "change_point_config");
  return ChangePointConfig{
      .presence_threshold = require_f64(obj, "presence_threshold"),
      .hysteresis_frames = require_i32(obj, "hysteresis_frames"),
      .change_threshold = require_i32(obj, "change_threshold"),
      .enable_ssim_verify = require_bool(obj, "enable_ssim_verify"),
      .ssim_threshold = require_f64(obj, "ssim_threshold"),
      .ssim_window_size = require_i32(obj, "ssim_window_size"),
      .enable_ssim_patrol = require_bool(obj, "enable_ssim_patrol"),
      .ssim_patrol_interval = require_i32(obj, "ssim_patrol_interval"),
      .ssim_patrol_threshold = require_f64(obj, "ssim_patrol_threshold"),
      .ssim_patrol_use_mask = require_bool(obj, "ssim_patrol_use_mask"),
  };
}

ChangepointEventGolden parse_event(const json& obj) {
  require_only_keys(obj, {"event_type", "timestamp_ms", "prev_end_ms"}, "event");
  ChangepointEventGolden e;
  e.event_type = require_string(obj, "event_type");
  e.timestamp_ms = require_i64(obj, "timestamp_ms");
  const auto& pe = obj.at("prev_end_ms");
  if (pe.is_null()) {
    e.prev_end_ms = std::nullopt;
  } else if (pe.is_number_integer() || pe.is_number_unsigned()) {
    e.prev_end_ms = pe.get<std::int64_t>();
  } else {
    throw std::runtime_error("prev_end_ms must be int or null");
  }
  return e;
}

ChangepointFrameGolden parse_frame(const json& obj) {
  require_only_keys(
      obj, {"timestamp_ms", "foreground_ratio", "dhash", "crop"}, "frame");
  ChangepointFrameGolden f;
  f.timestamp_ms = require_i64(obj, "timestamp_ms");
  f.foreground_ratio = require_f64(obj, "foreground_ratio");
  f.dhash = require_u64(obj, "dhash");
  const auto& c = obj.at("crop");
  if (c.is_null()) {
    f.crop_name = std::nullopt;
  } else if (c.is_string()) {
    f.crop_name = c.get<std::string>();
  } else {
    throw std::runtime_error("crop must be string or null");
  }
  return f;
}

ChangepointCropGolden parse_crop(const json& obj) {
  require_only_keys(obj,
                    {"name", "asset", "pixel_semantics", "width", "height",
                     "input_asset_sha256"},
                    "crop");
  return ChangepointCropGolden{
      .name = require_string(obj, "name"),
      .asset = require_string(obj, "asset"),
      .pixel_semantics = require_string(obj, "pixel_semantics"),
      .input_asset_sha256 = require_string(obj, "input_asset_sha256"),
      .width = require_i32(obj, "width"),
      .height = require_i32(obj, "height"),
  };
}

ChangepointScenarioGolden parse_scenario(const json& obj) {
  require_only_keys(
      obj, {"name", "change_point_config", "frames", "events"}, "scenario");
  ChangepointScenarioGolden s;
  s.name = require_string(obj, "name");
  s.change_point_config = parse_change_point(obj.at("change_point_config"));
  if (!obj.at("frames").is_array() || !obj.at("events").is_array()) {
    throw std::runtime_error("scenario frames/events must be arrays");
  }
  for (const auto& fr : obj.at("frames")) {
    s.frames.push_back(parse_frame(fr));
  }
  for (const auto& ev : obj.at("events")) {
    s.events.push_back(parse_event(ev));
  }
  return s;
}

}  // namespace

ChangepointGolden load_changepoint_golden(const std::filesystem::path& path) {
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
  if (!root.contains("kind") || root.at("kind") != "changepoint") {
    throw std::runtime_error("kind must be changepoint");
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
  if (oj.contains("change_point_config_fingerprint") &&
      oj.at("change_point_config_fingerprint").is_string()) {
    oracle.config_fingerprint =
        oj.at("change_point_config_fingerprint").get<std::string>();
  }

  ChangepointGolden out;
  out.oracle = std::move(oracle);
  if (root.contains("crops") && root.at("crops").is_array()) {
    for (const auto& c : root.at("crops")) {
      out.crops.push_back(parse_crop(c));
    }
  }
  for (const auto& s : root.at("scenarios")) {
    out.scenarios.push_back(parse_scenario(s));
  }
  return out;
}

std::optional<std::string> diff_changepoint_events(
    const std::vector<ChangepointEventGolden>& candidate,
    const std::vector<ChangepointEventGolden>& expected,
    const std::string& scenario_name,
    const OracleMeta* oracle) {
  std::ostringstream body;
  if (candidate.size() != expected.size()) {
    body << "  event count: golden=" << expected.size()
         << " candidate=" << candidate.size() << "\n";
  }
  const std::size_t n = std::min(candidate.size(), expected.size());
  for (std::size_t i = 0; i < n; ++i) {
    const auto& c = candidate[i];
    const auto& e = expected[i];
    if (c.event_type != e.event_type || c.timestamp_ms != e.timestamp_ms ||
        c.prev_end_ms != e.prev_end_ms) {
      body << "  event[" << i << "]:\n"
           << "    golden: " << e.event_type << " ts=" << e.timestamp_ms
           << " prev=";
      if (e.prev_end_ms) {
        body << *e.prev_end_ms;
      } else {
        body << "null";
      }
      body << "\n    candidate: " << c.event_type << " ts=" << c.timestamp_ms
           << " prev=";
      if (c.prev_end_ms) {
        body << *c.prev_end_ms;
      } else {
        body << "null";
      }
      body << "\n";
    }
  }
  const auto detail = body.str();
  if (detail.empty()) {
    return std::nullopt;
  }
  std::ostringstream msg;
  msg << "changepoint parity mismatch scenario=" << scenario_name << "\n";
  if (oracle != nullptr) {
    msg << "  oracle_commit: " << oracle->oracle_commit << "\n"
        << "  golden_schema_version: " << oracle->golden_schema_version << "\n";
  }
  msg << detail;
  return msg.str();
}

}  // namespace sublift::test_support
