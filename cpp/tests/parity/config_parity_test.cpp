#include <catch2/catch_test_macros.hpp>

#include "sublift/config.hpp"
#include "sublift/test_support.hpp"

#ifndef SUBLIFT_PARITY_GOLDEN_CONFIG
#error "SUBLIFT_PARITY_GOLDEN_CONFIG must be defined by CMake"
#endif

TEST_CASE("default Config matches frozen oracle golden", "[parity][config]") {
  const auto path = std::filesystem::path{SUBLIFT_PARITY_GOLDEN_CONFIG};
  const auto golden = sublift::test_support::load_config_golden(path);
  REQUIRE(golden.oracle.golden_schema_version == 1);
  REQUIRE_FALSE(golden.oracle.oracle_commit.empty());

  const auto diff = sublift::test_support::diff_config(
      sublift::kDefaultConfig, golden.config, &golden.oracle);
  if (diff.has_value()) {
    FAIL(*diff);
  }
}
