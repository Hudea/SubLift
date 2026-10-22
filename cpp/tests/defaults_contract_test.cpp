#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <string>

#include <nlohmann/json.hpp>

#include "sublift/cli/args.hpp"
#include "sublift/core/config.hpp"
#include "sublift/server/job_manager.hpp"

#ifndef SUBLIFT_CONTRACT_DEFAULTS
#error "SUBLIFT_CONTRACT_DEFAULTS must be defined by CMake"
#endif

TEST_CASE("request defaults match contracts/fixtures/defaults.json",
          "[defaults][contracts]") {
  std::ifstream in(SUBLIFT_CONTRACT_DEFAULTS);
  REQUIRE(in.is_open());
  nlohmann::json root;
  in >> root;
  const auto& canonical = root.at("canonical");
  const auto& preference = canonical.at("engine_preference");
  REQUIRE(preference.is_array());
  REQUIRE_FALSE(preference.empty());
  const auto preferred_engine = preference.front().get<std::string>();

  const auto confidence = canonical.at("confidence").get<double>();
  const auto script = canonical.at("script").get<std::string>();

  const sublift::cli::ExtractArgs cli{};
  CHECK(cli.engine == preferred_engine);
  CHECK(cli.script == script);
  CHECK(cli.confidence == confidence);

  const sublift::Config internal{};
  CHECK(internal.subtitle_script == script);

  const sublift::server::JobConfig job{};
  CHECK(job.engine == preferred_engine);
  CHECK(job.confidence_threshold == confidence);
}

TEST_CASE("job config parses the optional det pixel budget",
          "[defaults][contracts]") {
  const auto cfg = sublift::server::JobConfig::from_json(
      nlohmann::json{{"video_path", "v.mp4"}, {"det_pixel_budget", 600000.0}});
  REQUIRE(cfg.det_pixel_budget.has_value());
  CHECK(*cfg.det_pixel_budget == 600000.0);
}
