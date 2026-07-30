#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include "sublift/paddle.hpp"
#include "paddle_models.hpp"

TEST_CASE("Paddle Models - Type Parsing and Path Resolution", "[paddle][models]") {
  SECTION("Valid model_type strings") {
    using namespace sublift::paddle_detail;
    REQUIRE(parse_model_type("tiny") == ModelType::Tiny);
    REQUIRE(parse_model_type("small") == ModelType::Small);
    REQUIRE(parse_model_type("medium") == ModelType::Medium);
    REQUIRE(parse_model_type("SMALL") == ModelType::Small);
    REQUIRE(parse_model_type("Tiny") == ModelType::Tiny);
  }

  SECTION("Invalid model_type string throws invalid_argument") {
    using namespace sublift::paddle_detail;
    REQUIRE_THROWS_AS(parse_model_type("large"), std::invalid_argument);
    REQUIRE_THROWS_AS(parse_model_type("invalid_type"), std::invalid_argument);

    try {
      (void)parse_model_type("large");
    } catch (const std::invalid_argument& e) {
      std::string msg = e.what();
      REQUIRE(msg.find("未知 model_type") != std::string::npos);
    }
  }

  SECTION("PaddleOcrEngine validates model_type in options") {
    sublift::PaddleOcrOptions opts;
    opts.model_type = "invalid_spec";

    // Invalid model_type throws std::invalid_argument regardless of build status
    REQUIRE_THROWS_AS(sublift::PaddleOcrEngine(opts), std::invalid_argument);
  }

  SECTION("expand_user_path expands ~ prefix") {
    using namespace sublift::paddle_detail;
    auto path = expand_user_path("~/sublift-test-path");
    REQUIRE_FALSE(path.string().starts_with("~"));
  }

  SECTION("resolve_model_dir priority") {
    using namespace sublift::paddle_detail;

    // Custom dir takes precedence
    auto custom = resolve_model_dir("/tmp/custom_models");
    REQUIRE(custom.string() == "/tmp/custom_models");
  }

  SECTION("model validation fails honestly when Cls model is missing") {
    using namespace sublift::paddle_detail;
    const auto root =
        std::filesystem::temp_directory_path() /
        "sublift-paddle-model-validation";
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
