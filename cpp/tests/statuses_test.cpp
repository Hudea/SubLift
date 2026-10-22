#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "sublift/server/job_manager.hpp"

#ifndef SUBLIFT_CONTRACT_STATUSES
#error "SUBLIFT_CONTRACT_STATUSES must be defined by CMake"
#endif

TEST_CASE("contract job statuses round-trip through JobStatus",
          "[statuses][contracts]") {
  std::ifstream in(SUBLIFT_CONTRACT_STATUSES);
  REQUIRE(in.is_open());
  nlohmann::json root;
  in >> root;
  const auto& job = root.at("job");
  REQUIRE(job.is_array());
  REQUIRE_FALSE(job.empty());
  for (const auto& name : job) {
    const auto text = name.get<std::string>();
    CHECK(sublift::server::to_string(
              sublift::server::job_status_from_string(text)) == text);
  }
  // Legacy persisted state used "running"; it maps to extracting and is never
  // emitted again.
  CHECK(sublift::server::to_string(sublift::server::job_status_from_string(
            "running")) == "extracting");
}

TEST_CASE("task -> job status mapping matches contract",
          "[statuses][contracts]") {
  std::ifstream in(SUBLIFT_CONTRACT_STATUSES);
  REQUIRE(in.is_open());
  nlohmann::json root;
  in >> root;
  const auto& task = root.at("task");
  const auto& mapping = root.at("mapping");
  REQUIRE(task.is_array());
  REQUIRE(mapping.is_object());
  for (const auto& name : task) {
    const auto key = name.get<std::string>();
    REQUIRE(mapping.contains(key));
    const auto& expected = mapping.at(key);
    const auto mapped = sublift::server::task_status_to_job_status(key);
    if (expected.is_null()) {
      CHECK_FALSE(mapped.has_value());
    } else {
      REQUIRE(mapped.has_value());
      CHECK(*mapped == expected.get<std::string>());
    }
  }
}
