#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include "sublift/test_support.hpp"
#include "sublift/version.hpp"

TEST_CASE("sublift_core version is non-empty", "[smoke]") {
  REQUIRE_FALSE(sublift::version().empty());
}

TEST_CASE("nlohmann_json is usable", "[smoke]") {
  nlohmann::json j = {{"project", "sublift"}, {"phase", "6.0"}};
  REQUIRE(j.at("project") == "sublift");
  REQUIRE(j.at("phase") == "6.0");
}

TEST_CASE("test_support stub", "[smoke]") {
  REQUIRE(sublift::test_support::smoke_ok());
}
