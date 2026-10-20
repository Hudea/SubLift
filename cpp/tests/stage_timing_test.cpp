#include <catch2/catch_test_macros.hpp>

#include "sublift/pipeline/stage_timing.hpp"

#include <string>

using sublift::SharedStageTimingRegistry;
using sublift::StageTimingEntry;
using sublift::StageTimingRegistry;
using sublift::StageTimer;
using sublift::stage_timing_enabled;

// Phase 16 D01 (S0) measurement infrastructure. The enabled path
// (SUBLIFT_STAGE_TIMING=1) is process-cached by design (zero-cost static
// check on the product path), so it is covered by the end-to-end attribution
// runs recorded in the D01 evidence rather than by in-process env toggling.

TEST_CASE("StageTimingRegistry accumulates calls and totals per stage",
          "[stage_timing]") {
  StageTimingRegistry registry;
  REQUIRE(registry.empty());

  registry.add("signature", 1.5);
  registry.add("signature", 2.5);
  registry.add("ocr", 10.0);

  const auto& stages = registry.stages();
  REQUIRE(stages.size() == 2);
  REQUIRE(stages.at("signature").calls == 2);
  REQUIRE(stages.at("signature").total_ms == 4.0);
  REQUIRE(stages.at("ocr").calls == 1);
  REQUIRE(stages.at("ocr").total_ms == 10.0);
  REQUIRE_FALSE(registry.empty());

  registry.clear();
  REQUIRE(registry.empty());
}

TEST_CASE("StageTimer accumulates into the registry it is bound to",
          "[stage_timing]") {
  // StageTimer no-ops when stage timing is disabled (the default in the test
  // process); when enabled it adds exactly one entry per scope. Both paths
  // must leave the registry in a consistent state.
  StageTimingRegistry registry;
  {
    StageTimer timer(registry, "changepoint");
    (void)timer;
  }
  if (stage_timing_enabled()) {
    REQUIRE(registry.stages().at("changepoint").calls == 1);
  } else {
    REQUIRE(registry.empty());
  }

  SharedStageTimingRegistry shared;
  {
    StageTimer timer(shared, "paddle.det_infer");
    (void)timer;
  }
  const auto snapshot = shared.snapshot();
  if (stage_timing_enabled()) {
    REQUIRE(snapshot.at("paddle.det_infer").calls == 1);
  } else {
    REQUIRE(snapshot.empty());
  }
}

TEST_CASE("SharedStageTimingRegistry snapshot copies current state",
          "[stage_timing]") {
  SharedStageTimingRegistry registry;
  registry.add("extract_wait", 3.25);
  registry.add("extract_wait", 1.75);

  const auto first = registry.snapshot();
  REQUIRE(first.size() == 1);
  REQUIRE(first.at("extract_wait").calls == 2);
  REQUIRE(first.at("extract_wait").total_ms == 5.0);

  // Later mutations must not alias into an earlier snapshot.
  registry.add("extract_wait", 100.0);
  REQUIRE(first.at("extract_wait").total_ms == 5.0);
  REQUIRE(registry.snapshot().at("extract_wait").total_ms == 105.0);

  registry.clear();
  REQUIRE(registry.snapshot().empty());
}

TEST_CASE("stage timing is disabled by default in a clean environment",
          "[stage_timing]") {
  // The product path must carry no stage-timing cost unless the operator
  // opts in. The test process does not set SUBLIFT_STAGE_TIMING.
  const char* raw = std::getenv("SUBLIFT_STAGE_TIMING");
  if (raw == nullptr) {
    REQUIRE_FALSE(stage_timing_enabled());
  }
}
