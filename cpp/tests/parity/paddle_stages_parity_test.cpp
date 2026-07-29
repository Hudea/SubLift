#include <catch2/catch_test_macros.hpp>
#include <fstream>
#include <iostream>
#include <string>
#include <nlohmann/json.hpp>

#include "sublift/paddle.hpp"

#ifndef SUBLIFT_PARITY_GOLDEN_PADDLE_STAGES
#define SUBLIFT_PARITY_GOLDEN_PADDLE_STAGES "benchmark/parity/goldens/paddle/paddle_stages.v1.json"
#endif

TEST_CASE("Paddle stages parity golden verification", "[paddle][parity][stages]") {
  std::ifstream f(SUBLIFT_PARITY_GOLDEN_PADDLE_STAGES);
  REQUIRE(f.is_open());

  nlohmann::json j;
  f >> j;

  REQUIRE(j.contains("kind"));
  CHECK(j["kind"] == "paddle_stages");
  REQUIRE(j.contains("fingerprint"));
  CHECK(j["fingerprint"].contains("dependencies"));
  CHECK(j["fingerprint"]["dependencies"]["rapidocr"] == "3.9.2");

  REQUIRE(j.contains("cases"));
  const auto& cases = j["cases"];
  REQUIRE(cases.is_array());
  REQUIRE(cases.size() >= 4);

  // Check 10 stages structure on first case
  const auto& first_case = cases[0];
  REQUIRE(first_case.contains("stages"));
  const auto& stages = first_case["stages"];
  CHECK(stages.contains("1_global_preprocess"));
  CHECK(stages.contains("2_det_preprocess"));
  CHECK(stages.contains("3_det_infer"));
  CHECK(stages.contains("4_det_postprocess"));
  CHECK(stages.contains("5_perspective_crop"));
  CHECK(stages.contains("6_cls"));
  CHECK(stages.contains("7_rec_preprocess"));
  CHECK(stages.contains("8_rec_infer"));
  CHECK(stages.contains("9_rec_decode"));
  CHECK(stages.contains("10_filter_sort"));
}

TEST_CASE("PaddleOcrOptions dump_stages options test", "[paddle][options]") {
  sublift::PaddleOcrOptions opts;
  opts.dump_stages = true;
  opts.dump_out_dir = "/tmp/sublift_stage_dump";
  CHECK(opts.dump_stages == true);
  CHECK(opts.dump_out_dir == "/tmp/sublift_stage_dump");
}
