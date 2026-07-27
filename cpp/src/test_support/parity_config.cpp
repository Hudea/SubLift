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
  if (!v.is_number_integer()) {
    throw std::runtime_error(std::string("field not integer: ") + key);
  }
  // nlohmann get<int32_t>() silently truncates out-of-range int64; reject instead.
  const auto wide = v.get<std::int64_t>();
  if (wide < std::numeric_limits<std::int32_t>::min() ||
      wide > std::numeric_limits<std::int32_t>::max()) {
    throw std::runtime_error(std::string("int field out of int32 range: ") + key);
  }
  return static_cast<std::int32_t>(wide);
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

SignatureConfig parse_signature(const json& obj) {
  require_only_keys(obj, {"block_size_ratio", "adaptive_c", "hash_size"}, "signature");
  return SignatureConfig{
      .block_size_ratio = require_f64(obj, "block_size_ratio"),
      .adaptive_c = require_i32(obj, "adaptive_c"),
      .hash_size = require_i32(obj, "hash_size"),
  };
}

ChangePointConfig parse_change_point(const json& obj) {
  require_only_keys(obj,
                    {"presence_threshold", "hysteresis_frames", "change_threshold",
                     "enable_ssim_verify", "ssim_threshold", "ssim_window_size",
                     "enable_ssim_patrol", "ssim_patrol_interval",
                     "ssim_patrol_threshold", "ssim_patrol_use_mask"},
                    "change_point");
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

std::optional<SubtitleProfile> parse_profile(const json& v) {
  if (v.is_null()) {
    return std::nullopt;
  }
  if (!v.is_object()) {
    throw std::runtime_error("subtitle_profile must be null or object");
  }
  require_only_keys(v, {"script", "center_x", "center_y", "height", "y_min", "y_max"},
                    "subtitle_profile");
  return SubtitleProfile{
      .script = require_string(v, "script"),
      .center_x = require_i32(v, "center_x"),
      .center_y = require_i32(v, "center_y"),
      .height = require_i32(v, "height"),
      .y_min = require_i32(v, "y_min"),
      .y_max = require_i32(v, "y_max"),
  };
}

Config parse_config(const json& obj) {
  require_only_keys(obj, {"sample_fps",
                           "region_bottom_ratio",
                           "confidence_threshold",
                           "merge_gap_ms",
                           "min_duration_ms",
                           "ocr_anchor_delay_frames",
                           "drop_empty_text",
                           "subtitle_profile",
                           "subtitle_script",
                           "enable_line_select",
                           "low_conf_threshold",
                           "ocr_consensus_frames",
                           "line_select_min_score",
                           "line_select_min_script",
                           "signature",
                           "change_point"},
                    "config");
  Config cfg;
  cfg.sample_fps = require_f64(obj, "sample_fps");
  cfg.region_bottom_ratio = require_f64(obj, "region_bottom_ratio");
  cfg.confidence_threshold = require_f64(obj, "confidence_threshold");
  cfg.merge_gap_ms = require_i32(obj, "merge_gap_ms");
  cfg.min_duration_ms = require_i32(obj, "min_duration_ms");
  cfg.ocr_anchor_delay_frames = require_i32(obj, "ocr_anchor_delay_frames");
  cfg.drop_empty_text = require_bool(obj, "drop_empty_text");
  cfg.subtitle_profile = parse_profile(obj.at("subtitle_profile"));
  cfg.subtitle_script = require_string(obj, "subtitle_script");
  cfg.enable_line_select = require_bool(obj, "enable_line_select");
  cfg.low_conf_threshold = require_f64(obj, "low_conf_threshold");
  cfg.ocr_consensus_frames = require_i32(obj, "ocr_consensus_frames");
  cfg.line_select_min_score = require_f64(obj, "line_select_min_score");
  cfg.line_select_min_script = require_f64(obj, "line_select_min_script");
  cfg.signature = parse_signature(obj.at("signature"));
  cfg.change_point = parse_change_point(obj.at("change_point"));
  return cfg;
}

template <typename T>
void check_eq(std::ostringstream& out, const char* field, const T& cand, const T& gold) {
  if (cand != gold) {
    out << "  field: " << field << "\n"
        << "  golden: " << gold << "\n"
        << "  candidate: " << cand << "\n";
  }
}

void check_eq_bool(std::ostringstream& out, const char* field, bool cand, bool gold) {
  if (cand != gold) {
    out << "  field: " << field << "\n"
        << "  golden: " << (gold ? "true" : "false") << "\n"
        << "  candidate: " << (cand ? "true" : "false") << "\n";
  }
}

}  // namespace

ConfigGolden load_config_golden(const std::filesystem::path& path) {
  std::ifstream in(path);
  if (!in) {
    throw std::runtime_error("cannot open golden: " + path.string());
  }
  json root;
  in >> root;
  if (!root.is_object()) {
    throw std::runtime_error("golden root must be object");
  }
  if (!root.contains("golden_schema_version") || !root.at("golden_schema_version").is_number_integer() ||
      root.at("golden_schema_version").get<int>() != 1) {
    throw std::runtime_error("golden_schema_version must be 1");
  }
  if (!root.contains("kind") || root.at("kind") != "config") {
    throw std::runtime_error("kind must be config");
  }
  if (!root.contains("oracle") || !root.at("oracle").is_object()) {
    throw std::runtime_error("missing oracle object");
  }
  if (!root.contains("config") || !root.at("config").is_object()) {
    throw std::runtime_error("missing config object");
  }

  const auto& oracle_j = root.at("oracle");
  OracleMeta oracle;
  oracle.golden_schema_version = 1;
  oracle.oracle_commit = require_string(oracle_j, "oracle_commit");
  if (oracle.oracle_commit.empty()) {
    throw std::runtime_error("oracle_commit empty");
  }
  if (oracle_j.contains("oracle_branch") && oracle_j.at("oracle_branch").is_string()) {
    oracle.oracle_branch = oracle_j.at("oracle_branch").get<std::string>();
  }
  if (oracle_j.contains("config_fingerprint") && oracle_j.at("config_fingerprint").is_string()) {
    oracle.config_fingerprint = oracle_j.at("config_fingerprint").get<std::string>();
  }

  ConfigGolden out;
  out.oracle = std::move(oracle);
  out.config = parse_config(root.at("config"));
  return out;
}

std::optional<std::string> diff_config(const Config& candidate, const Config& golden,
                                       const OracleMeta* oracle) {
  if (candidate == golden) {
    return std::nullopt;
  }

  std::ostringstream body;
  check_eq(body, "sample_fps", candidate.sample_fps, golden.sample_fps);
  check_eq(body, "region_bottom_ratio", candidate.region_bottom_ratio,
           golden.region_bottom_ratio);
  check_eq(body, "confidence_threshold", candidate.confidence_threshold,
           golden.confidence_threshold);
  check_eq(body, "merge_gap_ms", candidate.merge_gap_ms, golden.merge_gap_ms);
  check_eq(body, "min_duration_ms", candidate.min_duration_ms, golden.min_duration_ms);
  check_eq(body, "ocr_anchor_delay_frames", candidate.ocr_anchor_delay_frames,
           golden.ocr_anchor_delay_frames);
  check_eq_bool(body, "drop_empty_text", candidate.drop_empty_text, golden.drop_empty_text);
  check_eq(body, "subtitle_script", candidate.subtitle_script, golden.subtitle_script);
  check_eq_bool(body, "enable_line_select", candidate.enable_line_select,
                golden.enable_line_select);
  check_eq(body, "low_conf_threshold", candidate.low_conf_threshold,
           golden.low_conf_threshold);
  check_eq(body, "ocr_consensus_frames", candidate.ocr_consensus_frames,
           golden.ocr_consensus_frames);
  check_eq(body, "line_select_min_score", candidate.line_select_min_score,
           golden.line_select_min_score);
  check_eq(body, "line_select_min_script", candidate.line_select_min_script,
           golden.line_select_min_script);

  if (candidate.subtitle_profile.has_value() != golden.subtitle_profile.has_value()) {
    body << "  field: subtitle_profile presence mismatch\n";
  } else if (candidate.subtitle_profile && golden.subtitle_profile) {
    const auto& c = *candidate.subtitle_profile;
    const auto& g = *golden.subtitle_profile;
    check_eq(body, "subtitle_profile.script", c.script, g.script);
    check_eq(body, "subtitle_profile.center_x", c.center_x, g.center_x);
    check_eq(body, "subtitle_profile.center_y", c.center_y, g.center_y);
    check_eq(body, "subtitle_profile.height", c.height, g.height);
    check_eq(body, "subtitle_profile.y_min", c.y_min, g.y_min);
    check_eq(body, "subtitle_profile.y_max", c.y_max, g.y_max);
  }

  check_eq(body, "signature.block_size_ratio", candidate.signature.block_size_ratio,
           golden.signature.block_size_ratio);
  check_eq(body, "signature.adaptive_c", candidate.signature.adaptive_c,
           golden.signature.adaptive_c);
  check_eq(body, "signature.hash_size", candidate.signature.hash_size,
           golden.signature.hash_size);

  check_eq(body, "change_point.presence_threshold",
           candidate.change_point.presence_threshold, golden.change_point.presence_threshold);
  check_eq(body, "change_point.hysteresis_frames", candidate.change_point.hysteresis_frames,
           golden.change_point.hysteresis_frames);
  check_eq(body, "change_point.change_threshold", candidate.change_point.change_threshold,
           golden.change_point.change_threshold);
  check_eq_bool(body, "change_point.enable_ssim_verify",
                candidate.change_point.enable_ssim_verify,
                golden.change_point.enable_ssim_verify);
  check_eq(body, "change_point.ssim_threshold", candidate.change_point.ssim_threshold,
           golden.change_point.ssim_threshold);
  check_eq(body, "change_point.ssim_window_size", candidate.change_point.ssim_window_size,
           golden.change_point.ssim_window_size);
  check_eq_bool(body, "change_point.enable_ssim_patrol",
                candidate.change_point.enable_ssim_patrol,
                golden.change_point.enable_ssim_patrol);
  check_eq(body, "change_point.ssim_patrol_interval",
           candidate.change_point.ssim_patrol_interval,
           golden.change_point.ssim_patrol_interval);
  check_eq(body, "change_point.ssim_patrol_threshold",
           candidate.change_point.ssim_patrol_threshold,
           golden.change_point.ssim_patrol_threshold);
  check_eq_bool(body, "change_point.ssim_patrol_use_mask",
                candidate.change_point.ssim_patrol_use_mask,
                golden.change_point.ssim_patrol_use_mask);

  const auto detail = body.str();
  if (detail.empty()) {
    return std::nullopt;
  }
  std::ostringstream msg;
  msg << "config parity mismatch\n";
  if (oracle != nullptr) {
    msg << "  oracle_commit: " << oracle->oracle_commit << "\n"
        << "  golden_schema_version: " << oracle->golden_schema_version << "\n";
  }
  msg << detail;
  return msg.str();
}

}  // namespace sublift::test_support
