#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include "sublift/models/model_bundle.hpp"

TEST_CASE("sublift_models - Direct sublift::models namespace tests", "[models][bundle]") {
  SECTION("Valid model_type strings") {
    using namespace sublift::models;
    REQUIRE(parse_model_type("tiny") == ModelType::Tiny);
    REQUIRE(parse_model_type("small") == ModelType::Small);
    REQUIRE(parse_model_type("medium") == ModelType::Medium);
    REQUIRE(parse_model_type("SMALL") == ModelType::Small);
    REQUIRE(parse_model_type("Tiny") == ModelType::Tiny);
  }

  SECTION("Invalid model_type string throws invalid_argument") {
    using namespace sublift::models;
    REQUIRE_THROWS_AS(parse_model_type("large"), std::invalid_argument);
    REQUIRE_THROWS_AS(parse_model_type("invalid_spec"), std::invalid_argument);
  }

  SECTION("expand_user_path expands ~ prefix") {
    using namespace sublift::models;
    auto path = expand_user_path("~/sublift-test-path");
    REQUIRE_FALSE(path.string().starts_with("~"));
  }

  SECTION("resolve_model_dir priority") {
    using namespace sublift::models;
    auto custom = resolve_model_dir("/tmp/custom_models");
    REQUIRE(custom.string() == "/tmp/custom_models");
  }

  SECTION("model validation fails honestly when Cls model is missing") {
    using namespace sublift::models;
    const auto root =
        std::filesystem::temp_directory_path() /
        "sublift-models-validation-test";
    std::filesystem::create_directories(root);
    const auto paths = get_expected_model_paths(root, ModelType::Small);
    std::ofstream(paths.det_path).put('\n');
    std::ofstream(paths.rec_path).put('\n');
    std::ofstream(paths.keys_path).put('\n');

    std::string error;
    CHECK_FALSE(validate_model_paths(paths, &error));
    CHECK(error.find(paths.cls_path.filename().string()) != std::string::npos);

    std::filesystem::remove_all(root);
  }
}
