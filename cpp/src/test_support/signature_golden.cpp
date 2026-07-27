#include "sublift/test_support.hpp"

#include <algorithm>
#include <cmath>
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

/// L1 epsilon per parity-contract.md §4: abs(a-b) <= 1e-9 + 1e-6*max(|a|,|b|).
[[nodiscard]] bool fg_ratio_eq(double a, double b) noexcept {
  return std::abs(a - b) <= 1e-9 + 1e-6 * std::max(std::abs(a), std::abs(b));
}

SignatureConfig parse_signature_config(const json& obj) {
  require_only_keys(obj, {"block_size_ratio", "adaptive_c", "hash_size"},
                    "signature_config");
  return SignatureConfig{
      .block_size_ratio = require_f64(obj, "block_size_ratio"),
      .adaptive_c = require_i32(obj, "adaptive_c"),
      .hash_size = require_i32(obj, "hash_size"),
  };
}

SignatureFixtureGolden parse_fixture(const json& obj) {
  require_only_keys(obj,
                    {"name", "pixel_semantics", "asset", "input_asset_sha256",
                     "width", "height", "timestamp_ms", "fg_ratio", "dhash"},
                    "signature fixture");
  return SignatureFixtureGolden{
      .name = require_string(obj, "name"),
      .pixel_semantics = require_string(obj, "pixel_semantics"),
      .asset = require_string(obj, "asset"),
      .input_asset_sha256 = require_string(obj, "input_asset_sha256"),
      .width = require_i32(obj, "width"),
      .height = require_i32(obj, "height"),
      .timestamp_ms = require_i64(obj, "timestamp_ms"),
      .fg_ratio = require_f64(obj, "fg_ratio"),
      .dhash = require_u64(obj, "dhash"),
  };
}

}  // namespace

SignatureGolden load_signature_golden(const std::filesystem::path& path) {
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
  if (!root.contains("kind") || root.at("kind") != "signature") {
    throw std::runtime_error("kind must be signature");
  }
  if (!root.contains("oracle") || !root.at("oracle").is_object()) {
    throw std::runtime_error("missing oracle object");
  }
  if (!root.contains("signature_config") || !root.at("signature_config").is_object()) {
    throw std::runtime_error("missing signature_config object");
  }
  if (!root.contains("fixtures") || !root.at("fixtures").is_array()) {
    throw std::runtime_error("missing fixtures array");
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
  // Signature golden stores signature_config_fingerprint; reuse the field.
  if (oj.contains("signature_config_fingerprint") &&
      oj.at("signature_config_fingerprint").is_string()) {
    oracle.config_fingerprint = oj.at("signature_config_fingerprint").get<std::string>();
  }

  SignatureGolden out;
  out.oracle = std::move(oracle);
  out.signature_config = parse_signature_config(root.at("signature_config"));
  for (const auto& f : root.at("fixtures")) {
    out.fixtures.push_back(parse_fixture(f));
  }
  return out;
}

std::optional<std::string> diff_signature(const FrameSignature& candidate,
                                          const SignatureFixtureGolden& expected,
                                          const OracleMeta* oracle) {
  std::ostringstream body;
  if (candidate.timestamp_ms != expected.timestamp_ms) {
    body << "  field: timestamp_ms (L0)\n"
         << "  golden: " << expected.timestamp_ms << "\n"
         << "  candidate: " << candidate.timestamp_ms << "\n";
  }
  if (candidate.dhash != expected.dhash) {
    body << "  field: dhash (L0)\n"
         << "  golden: 0x" << std::hex << expected.dhash << "\n"
         << "  candidate: 0x" << candidate.dhash << std::dec << "\n";
  }
  if (!fg_ratio_eq(candidate.foreground_ratio, expected.fg_ratio)) {
    body << "  field: fg_ratio (L1 epsilon)\n"
         << "  golden: " << expected.fg_ratio << "\n"
         << "  candidate: " << candidate.foreground_ratio << "\n";
  }

  const auto detail = body.str();
  if (detail.empty()) {
    return std::nullopt;
  }
  std::ostringstream msg;
  msg << "signature parity mismatch (fixture: " << expected.name << ")\n";
  if (oracle != nullptr) {
    msg << "  oracle_commit: " << oracle->oracle_commit << "\n"
        << "  golden_schema_version: " << oracle->golden_schema_version << "\n";
  }
  msg << detail;
  return msg.str();
}

}  // namespace sublift::test_support
