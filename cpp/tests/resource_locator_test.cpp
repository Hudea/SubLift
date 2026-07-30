#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include "sublift/models/resource_locator.hpp"
#include "sublift/paddle.hpp"

TEST_CASE("ResourceLocator - Paddle Model Bundle Probe & Locate", "[models][resource_locator]") {
  sublift::models::ResourceLocator locator;

  SECTION("Explicit custom dir override takes highest precedence") {
    const auto temp_dir =
        std::filesystem::temp_directory_path() / "sublift-resource-locator-override-test";
    std::filesystem::create_directories(temp_dir);

    const auto expected =
        sublift::models::get_expected_model_paths(temp_dir, sublift::models::ModelType::Small);
    std::ofstream(expected.det_path).put('\n');
    std::ofstream(expected.cls_path).put('\n');
    std::ofstream(expected.rec_path).put('\n');
    std::ofstream(expected.keys_path).put('\n');

    auto result = locator.probe_model_bundle(temp_dir.string(), sublift::models::ModelType::Small);
    REQUIRE(result.found);
    REQUIRE(result.source == sublift::models::ResourceSource::ExplicitOverride);
    REQUIRE(result.value.det_path == expected.det_path);

    std::filesystem::remove_all(temp_dir);
  }

  SECTION("Fail-closed when model files missing in custom dir") {
    const auto empty_dir =
        std::filesystem::temp_directory_path() / "sublift-resource-locator-empty-test";
    std::filesystem::create_directories(empty_dir);

    auto result = locator.probe_model_bundle(empty_dir.string(), sublift::models::ModelType::Small);
    REQUIRE_FALSE(result.found);
    REQUIRE(result.source == sublift::models::ResourceSource::ExplicitOverride);
    REQUIRE_FALSE(result.error_msg.empty());
    REQUIRE(result.error_msg.find("缺少 Paddle 模型文件") != std::string::npos);

    std::filesystem::remove_all(empty_dir);
  }

  SECTION("Probe == Construct consistency assertion") {
    const auto empty_dir =
        std::filesystem::temp_directory_path() / "sublift-resource-locator-probe-construct-test";
    std::filesystem::create_directories(empty_dir);

    auto probe_res = locator.probe_model_bundle(empty_dir.string(), sublift::models::ModelType::Small);
    auto locate_res = locator.locate_model_bundle(empty_dir.string(), sublift::models::ModelType::Small);

    REQUIRE(probe_res.found == locate_res.found);
    REQUIRE(probe_res.source == locate_res.source);
    REQUIRE(probe_res.error_msg == locate_res.error_msg);

    // Verify PaddleOcrEngine constructor throws identical error message when probe fails
    sublift::PaddleOcrOptions opts;
    opts.model_root_dir = empty_dir.string();
    opts.model_type = "small";

    try {
      sublift::PaddleOcrEngine engine(opts);
      FAIL("PaddleOcrEngine expected to throw on missing model bundle");
    } catch (const std::runtime_error& e) {
#if defined(SUBLIFT_HAS_PADDLE) && SUBLIFT_HAS_PADDLE
      REQUIRE(std::string(e.what()) == probe_res.error_msg);
#else
      REQUIRE_FALSE(std::string(e.what()).empty());
#endif
    }

    std::filesystem::remove_all(empty_dir);
  }
}

TEST_CASE("ResourceLocator - Executable and Library Locators", "[models][resource_locator]") {
  sublift::models::ResourceLocator locator;

  SECTION("Locate FFmpeg executable in environment or system path") {
    auto res = locator.locate_ffmpeg_executable();
    if (res.found) {
      REQUIRE(std::filesystem::exists(res.value));
    }
  }

  SECTION("Locate ONNX Runtime custom override path") {
    auto res = locator.locate_onnxruntime_library("/non/existent/path/libonnxruntime.dylib");
    REQUIRE_FALSE(res.found);
    REQUIRE(res.source == sublift::models::ResourceSource::ExplicitOverride);
    REQUIRE(res.error_msg.find("ONNX Runtime library not found") != std::string::npos);
  }
}
