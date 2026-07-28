#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "sublift/config.hpp"
#include "sublift/extractor.hpp"
#include "sublift/models.hpp"
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

// ---------------------------------------------------------------------------
// Pipeline golden (feat-06205)
// ---------------------------------------------------------------------------

struct PipelineFrameGolden {
  std::int64_t ts{0};
  std::string pattern;            // "empty" | "subA" | "subB" | "subCyan"
  std::string action{"feed"};     // "feed" | "cancel" | "finalize"
};

struct PipelineMockOcrGolden {
  std::string mode;  // "fixed" | "sequence"
  struct Line {
    std::string text;
    double confidence{0.0};
    OcrCropBox box{};
  };
  struct Result {
    std::string text;
    double confidence{0.0};
    std::vector<Line> lines;
  };
  std::vector<Result> results;
};

struct PipelineSegmentEventGolden {
  std::int64_t start_ms{0};
  std::int64_t end_ms{0};
  std::optional<std::int64_t> anchor_ts{};
  std::vector<std::int64_t> fallback_ts{};
};

struct PipelineEntryGolden {
  std::int64_t start_ms{0};
  std::int64_t end_ms{0};
  std::string text;
  double confidence{0.0};
};

struct PipelineScenarioGolden {
  std::string name;
  Config config;  // default + scenario overrides applied
  std::vector<PipelineFrameGolden> frames;
  std::string detector_kind;  // "fixed_full"
  PipelineMockOcrGolden mock_ocr;
  std::vector<PipelineSegmentEventGolden> expected_segment_events;
  std::vector<PipelineEntryGolden> expected_raw_entries;
  int expected_ocr_calls{0};
  std::vector<PipelineEntryGolden> expected_final_entries;
};

struct PipelineGolden {
  OracleMeta oracle;  // config_fingerprint holds default-config fingerprint
  std::vector<PipelineScenarioGolden> scenarios;
};

/// Load pipeline golden envelope (schema v1). Throws std::runtime_error.
[[nodiscard]] PipelineGolden load_pipeline_golden(
    const std::filesystem::path& path);

/// Compare candidate segment events / raw entries / final entries / ocr_calls
/// against expected. nullopt if all equal; else human-readable mismatch.
[[nodiscard]] std::optional<std::string> diff_pipeline(
    const std::vector<PipelineSegmentEventGolden>& cand_events,
    const std::vector<PipelineEntryGolden>& cand_raw,
    const std::vector<PipelineEntryGolden>& cand_final,
    int cand_ocr_calls,
    const PipelineScenarioGolden& expected,
    const OracleMeta* oracle = nullptr);

// ---------------------------------------------------------------------------
// Extractor golden (feat-06305)
// ---------------------------------------------------------------------------

struct ExtractorValidateCropGolden {
  std::string name;
  std::optional<SourceBox> crop;
  std::int32_t source_width{0};
  std::int32_t source_height{0};
  bool expected_ok{true};
  std::optional<std::string> expected_error_contains;
};

struct ExtractorBuildVfGolden {
  std::string name;
  double fps{1.0};
  std::optional<SourceBox> crop;
  std::string expected_vf;
};

struct ExtractorAssessDisplayGolden {
  std::string name;
  nlohmann::json stream_json;
  bool expected_ok{true};
  std::optional<std::string> expected_note;
};

struct ExtractorPlanFrameIoGolden {
  std::string name;
  std::optional<SourceBox> region_box;
  std::string mode;
  std::optional<SourceFrameInfo> source;
  std::string on_unvalidated_transform{"fallback_full"};
  bool expected_ok{true};
  std::optional<std::string> expected_error_contains;
  std::optional<SourceBox> expected_output_crop;
  std::string expected_output_mode;
  std::string expected_detector_type;
  std::optional<std::string> expected_fallback_reason;
};

struct ExtractorSampledPixelGolden {
  std::int32_t x{0};
  std::int32_t y{0};
  std::array<int, 3> rgb{0, 0, 0};
};

struct ExtractorFrameGolden {
  int frame_index{0};
  std::int64_t timestamp_ms{0};
  std::vector<ExtractorSampledPixelGolden> sampled_pixels;
};

struct ExtractorExtractScenarioGolden {
  std::string name;
  double fps{1.0};
  std::optional<SourceBox> crop;
  bool cancel_before_extract{false};
  bool expected_ok{true};
  std::optional<std::string> expected_error_contains;
  int expected_frame_count{0};
  std::vector<std::int64_t> expected_timestamps_ms;
  std::int32_t expected_width{0};
  std::int32_t expected_height{0};
  std::vector<ExtractorFrameGolden> frames;
};

struct ExtractorGolden {
  OracleMeta oracle;
  std::vector<ExtractorValidateCropGolden> pure_validate_crop;
  std::vector<ExtractorBuildVfGolden> pure_build_vf;
  std::vector<ExtractorAssessDisplayGolden> pure_assess_display;
  std::vector<ExtractorPlanFrameIoGolden> pure_plan_frame_io;
  std::vector<ExtractorExtractScenarioGolden> extract_scenarios;
};

/// Load extractor golden envelope (schema v1). Throws std::runtime_error on error.
[[nodiscard]] ExtractorGolden load_extractor_golden(
    const std::filesystem::path& path);

// ---------------------------------------------------------------------------
// Vision golden (feat-06405)
// ---------------------------------------------------------------------------

struct VisionBoxTestCase {
  std::string name;
  double nx{0.0};
  double ny{0.0};
  double nw{0.0};
  double nh{0.0};
  std::int32_t image_width{0};
  std::int32_t image_height{0};
  OcrCropBox expected_box;
};

struct VisionClampTestCase {
  std::string name;
  std::int32_t x{0};
  std::int32_t y{0};
  std::int32_t w{0};
  std::int32_t h{0};
  std::int32_t image_width{0};
  std::int32_t image_height{0};
  OcrCropBox expected_box;
};

struct VisionSortTestCase {
  std::string name;
  std::vector<OcrLine> input_lines;
  std::vector<OcrLine> expected_lines;
};

/// Frozen empty OcrResult structure (perform-fail / no-obs / all-blank).
struct VisionEmptyResultCase {
  std::string name;
  std::string expected_text;
  double expected_confidence{0.0};
  std::size_t expected_line_count{0};
};

struct VisionGolden {
  OracleMeta oracle;
  std::string macos_version;
  bool vision_available{false};
  std::string vision_note;
  std::vector<std::string> default_languages;
  std::vector<VisionBoxTestCase> box_cases;
  std::vector<VisionClampTestCase> clamp_cases;
  std::vector<VisionSortTestCase> sort_cases;
  std::vector<VisionEmptyResultCase> empty_result_cases;
};

/// Load vision golden envelope (schema v1). Throws std::runtime_error on error.
[[nodiscard]] VisionGolden load_vision_golden(const std::filesystem::path& path);

}  // namespace sublift::test_support
