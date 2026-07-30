#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <set>
#include <string>

#include <nlohmann/json.hpp>

#include "sublift/diagnostics/paddle_stage_trace.hpp"
#include "sublift/paddle.hpp"

#ifndef SUBLIFT_PARITY_GOLDEN_PADDLE_STAGES
#define SUBLIFT_PARITY_GOLDEN_PADDLE_STAGES \
  "benchmark/parity/goldens/paddle/paddle_stages.v2.json"
#endif

namespace {

void require_tensor_summary(const nlohmann::json& tensor) {
  REQUIRE(tensor.is_object());
  REQUIRE(tensor.contains("shape"));
  REQUIRE(tensor["shape"].is_array());
  REQUIRE(tensor.contains("count"));
  REQUIRE(tensor["count"].is_number_unsigned());
  REQUIRE(tensor.contains("sha256"));
  REQUIRE(tensor["sha256"].is_string());
  CHECK(tensor["sha256"].get<std::string>().starts_with("sha256:"));
}

}  // namespace

TEST_CASE(
    "Paddle stage golden contains real dual-runtime traces",
    "[paddle][parity][stages]") {
  std::ifstream stream(SUBLIFT_PARITY_GOLDEN_PADDLE_STAGES);
  REQUIRE(stream.is_open());

  nlohmann::json golden;
  stream >> golden;

  REQUIRE(golden["schema_version"] == 2);
  REQUIRE(golden["kind"] == "paddle_stage_parity");
  REQUIRE(golden.contains("oracle_commit"));
  REQUIRE(golden["oracle_commit"].is_string());
  REQUIRE(golden["oracle_commit"].get<std::string>().size() == 40);

  const auto& fingerprint = golden["fingerprint"];
  REQUIRE(fingerprint["dependencies"]["rapidocr"] == "3.9.2");
  REQUIRE(fingerprint["dependencies"]["onnxruntime"] == "1.28.0");
  REQUIRE(
      fingerprint["model"]["det_file"] ==
      "PP-OCRv6_det_small.onnx");
  REQUIRE(
      fingerprint["model"]["rec_file"] ==
      "PP-OCRv6_rec_small.onnx");
  CHECK(
      fingerprint["parameters"]["det"]["limit_type"] ==
      "min");
  CHECK(
      fingerprint["parameters"]["det"]["limit_side_len"] ==
      736);

  const auto& cases = golden["cases"];
  REQUIRE(cases.is_array());
  REQUIRE(cases.size() >= 8);

  const std::set<std::string> expected_stage_names = {
      "1_global_preprocess",
      "2_det_preprocess",
      "3_det_infer",
      "4_det_postprocess",
      "5_perspective_crop",
      "6_cls",
      "7_rec_preprocess",
      "8_rec_infer",
      "9_rec_decode",
      "10_output",
  };

  bool saw_non_empty_oracle = false;
  bool saw_candidate_trace = false;
  for (const auto& one_case : cases) {
    REQUIRE(one_case.contains("asset_sha256"));
    CHECK(
        one_case["asset_sha256"].get<std::string>().starts_with("sha256:"));
    for (const auto runtime_name : {"oracle", "candidate"}) {
      const auto& trace = one_case[runtime_name];
      REQUIRE(trace.contains("stages"));
      std::set<std::string> actual_stage_names;
      for (const auto& [name, value] : trace["stages"].items()) {
        (void)value;
        actual_stage_names.insert(name);
      }
      CHECK(actual_stage_names == expected_stage_names);
      REQUIRE(trace["counts"]["recognize_calls"] == 1);
    }

    const auto& oracle_stages = one_case["oracle"]["stages"];
    const auto& candidate_stages = one_case["candidate"]["stages"];
    if (!oracle_stages["2_det_preprocess"]["tensor"].is_null()) {
      require_tensor_summary(
          oracle_stages["2_det_preprocess"]["tensor"]);
      saw_non_empty_oracle = true;
    }
    if (!candidate_stages["2_det_preprocess"]["tensor"].is_null()) {
      require_tensor_summary(
          candidate_stages["2_det_preprocess"]["tensor"]);
      saw_candidate_trace = true;
    }
  }
  CHECK(saw_non_empty_oracle);
  CHECK(saw_candidate_trace);
}

TEST_CASE(
    "Paddle stage trace is opt-in and resettable",
    "[paddle][options][stages]") {
  sublift::PaddleOcrOptions defaults;
  CHECK_FALSE(defaults.dump_stages);
  CHECK(defaults.stage_trace == nullptr);
  CHECK(defaults.intra_op_threads == 4);
  CHECK(defaults.cls_batch_size == 6);
  CHECK(defaults.rec_batch_size == 6);

  sublift::PaddleStageTrace trace;
  trace.rec_call_count = 7;
  trace.det_input.values = {1.0f, 2.0f};
  trace.clear();
  CHECK(trace.schema_version == 1);
  CHECK(trace.rec_call_count == 0);
  CHECK(trace.det_input.values.empty());
}
