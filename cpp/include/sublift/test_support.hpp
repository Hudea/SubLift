#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "sublift/config.hpp"
#include "sublift/signature.hpp"

namespace sublift::test_support {

/// Stub used by smoke tests.
[[nodiscard]] bool smoke_ok() noexcept;

/// SHA-256 of `data` as `sha256:` + lowercase hex (parity golden convention).
[[nodiscard]] std::string sha256_prefixed(const std::uint8_t* data,
                                          std::size_t len);

/// UTF-8 NFC normalize (parity-contract §5.3). On Apple uses CFStringNormalize;
/// other platforms return input unchanged (goldens stay precomposed CJK).
[[nodiscard]] std::string utf8_nfc(std::string_view text);

[[nodiscard]] inline std::string sha256_prefixed(
    const std::vector<std::uint8_t>& bytes) {
  return sha256_prefixed(bytes.data(), bytes.size());
}

struct OracleMeta {
  std::string oracle_commit;
  std::optional<std::string> oracle_branch;
  std::string config_fingerprint;
  int golden_schema_version{0};
};

struct ConfigGolden {
  OracleMeta oracle;
  Config config;
};

/// Load config golden envelope (schema v1). Throws std::runtime_error on error.
[[nodiscard]] ConfigGolden load_config_golden(const std::filesystem::path& path);

/// Field-by-field diff; nullopt if equal. Message includes oracle_commit + schema.
[[nodiscard]] std::optional<std::string> diff_config(
    const Config& candidate,
    const Config& golden,
    const OracleMeta* oracle = nullptr);

// ---------------------------------------------------------------------------
// Signature golden (feat-06101)
// ---------------------------------------------------------------------------

/// One fixture row in a signature golden (single frame, since compute_signature
/// is stateless). `pixel_semantics` is "rgb24" | "bgr24_as_rgb_gray_quirk" |
/// "gray8" and conveys how the C++ side must tag the loaded bytes.
struct SignatureFixtureGolden {
  std::string name;
  std::string pixel_semantics;
  std::string asset;  // relative .rgb filename under fixtures/signature/
  std::string input_asset_sha256;
  std::int32_t width{0};
  std::int32_t height{0};
  std::int64_t timestamp_ms{0};
  double fg_ratio{0.0};
  std::uint64_t dhash{0};
};

struct SignatureGolden {
  OracleMeta oracle;  // config_fingerprint holds signature_config_fingerprint
  SignatureConfig signature_config;
  std::vector<SignatureFixtureGolden> fixtures;
};

/// Load signature golden envelope (schema v1). Throws std::runtime_error on error.
[[nodiscard]] SignatureGolden load_signature_golden(
    const std::filesystem::path& path);

/// L0 (timestamp_ms, dhash exact) + L1 (fg_ratio abs_rel epsilon) diff.
/// nullopt if equal; otherwise a human-readable mismatch message naming the
/// fixture. See parity-contract.md §4.
[[nodiscard]] std::optional<std::string> diff_signature(
    const FrameSignature& candidate,
    const SignatureFixtureGolden& expected,
    const OracleMeta* oracle = nullptr);

// ---------------------------------------------------------------------------
// Changepoint golden (feat-06102)
// ---------------------------------------------------------------------------

struct ChangepointEventGolden {
  std::string event_type;  // "IN" | "OUT" | "CHANGE"
  std::int64_t timestamp_ms{0};
  std::optional<std::int64_t> prev_end_ms{};
};

struct ChangepointFrameGolden {
  std::int64_t timestamp_ms{0};
  double foreground_ratio{0.0};
  std::uint64_t dhash{0};
  std::optional<std::string> crop_name{};
};

struct ChangepointCropGolden {
  std::string name;
  std::string asset;
  std::string pixel_semantics;
  std::string input_asset_sha256;
  std::int32_t width{0};
  std::int32_t height{0};
};

struct ChangepointScenarioGolden {
  std::string name;
  ChangePointConfig change_point_config{};
  std::vector<ChangepointFrameGolden> frames;
  std::vector<ChangepointEventGolden> events;
};

struct ChangepointGolden {
  OracleMeta oracle;
  std::vector<ChangepointCropGolden> crops;
  std::vector<ChangepointScenarioGolden> scenarios;
};

[[nodiscard]] ChangepointGolden load_changepoint_golden(
    const std::filesystem::path& path);

/// L0 exact event-list compare. nullopt if equal.
[[nodiscard]] std::optional<std::string> diff_changepoint_events(
    const std::vector<ChangepointEventGolden>& candidate,
    const std::vector<ChangepointEventGolden>& expected,
    const std::string& scenario_name,
    const OracleMeta* oracle = nullptr);

}  // namespace sublift::test_support
