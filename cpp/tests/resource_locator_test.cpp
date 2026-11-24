#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>

#include "resource_locator.hpp"
#include "model_bundle.hpp"

namespace {

struct EnvGuard {
  std::string name;
  std::string old_val;
  bool had_val{false};

  explicit EnvGuard(std::string var) : name(std::move(var)) {
    if (const char* v = std::getenv(name.c_str())) {
      old_val = v;
      had_val = true;
    }
  }

  ~EnvGuard() {
    if (had_val) {
      ::setenv(name.c_str(), old_val.c_str(), 1);
    } else {
      ::unsetenv(name.c_str());
    }
  }
};

}  // namespace

TEST_CASE("ResourceLocator - Paddle Model Bundle Fail-Closed Validation", "[models][resource_locator]") {
  using namespace sublift::adapters::paddle::detail;
  ResourceLocator locator;

  const auto temp_dir =
      std::filesystem::temp_directory_path() / "sublift-locator-test-dir";
  std::filesystem::create_directories(temp_dir);

  const auto expected = get_expected_model_paths(temp_dir, ModelType::Small);
  std::ofstream(expected.det_path) << "dummy det content";
  std::ofstream(expected.cls_path) << "dummy cls content";
  std::ofstream(expected.rec_path) << "dummy rec content";
  std::ofstream(expected.keys_path) << "dummy keys content";

  SECTION("require_digest=false successfully finds models without digest check") {
    auto res = locator.probe_model_bundle(
        temp_dir.string(), ModelType::Small, false);
    REQUIRE(res.found);
    REQUIRE(res.source == ResourceSource::ExplicitOverride);
    REQUIRE(res.value.det_path == expected.det_path);
  }

  SECTION("require_digest=true fails closed if model type != Small") {
    auto res_tiny = locator.probe_model_bundle(
        temp_dir.string(), ModelType::Tiny, true);
    REQUIRE_FALSE(res_tiny.found);
    REQUIRE(res_tiny.error_msg.find("is not pinned in resources/manifest.json") != std::string::npos);

    auto res_med = locator.probe_model_bundle(
        temp_dir.string(), ModelType::Medium, true);
    REQUIRE_FALSE(res_med.found);
    REQUIRE(res_med.error_msg.find("is not pinned in resources/manifest.json") != std::string::npos);
  }

  SECTION("require_digest=true fails closed if manifest is missing") {
    EnvGuard guard("SUBLIFT_NATIVE_MANIFEST");
    ::setenv("SUBLIFT_NATIVE_MANIFEST", "/nonexistent/path/to/manifest.json", 1);

    auto res = locator.probe_model_bundle(
        temp_dir.string(), ModelType::Small, true);
    REQUIRE_FALSE(res.found);
    REQUIRE_FALSE(res.error_msg.empty());
  }

  SECTION("require_digest=true fails closed if manifest is invalid JSON") {
    const auto bad_manifest_path = temp_dir / "bad_manifest.json";
    {
      std::ofstream f(bad_manifest_path);
      f << "{ not valid json";
    }

    EnvGuard guard("SUBLIFT_NATIVE_MANIFEST");
    ::setenv("SUBLIFT_NATIVE_MANIFEST", bad_manifest_path.c_str(), 1);

    auto res = locator.probe_model_bundle(
        temp_dir.string(), ModelType::Small, true);
    REQUIRE_FALSE(res.found);
    REQUIRE_FALSE(res.error_msg.empty());
  }

  SECTION("require_digest=true fails closed if digest is tampered/mismatched") {
    // When using standard manifest (from repo resources), dummy file SHA-256 won't match
    auto res = locator.probe_model_bundle(
        temp_dir.string(), ModelType::Small, true);
    REQUIRE_FALSE(res.found);
    REQUIRE(res.error_msg.find("SHA-256 不匹配") != std::string::npos);

    // Also locate_model_bundle must throw std::runtime_error
    REQUIRE_THROWS_AS(
        locator.locate_model_bundle(temp_dir.string(), ModelType::Small, true),
        std::runtime_error);
  }

  std::filesystem::remove_all(temp_dir);
}

TEST_CASE("ResourceLocator - Executable and Library Locators", "[models][resource_locator]") {
  using namespace sublift::adapters::paddle::detail;
  ResourceLocator locator;

  SECTION("Locate FFmpeg executable in environment or system path") {
    auto res = locator.locate_ffmpeg_executable();
    if (res.found) {
      REQUIRE(std::filesystem::exists(res.value));
    }
  }

  SECTION("Locate ffprobe executable in environment or system path") {
    auto res = locator.locate_ffprobe_executable();
    if (res.found) {
      REQUIRE(std::filesystem::exists(res.value));
    }
  }

  SECTION("Locate ONNX Runtime custom override path") {
    auto res = locator.locate_onnxruntime_library("/non/existent/path/libonnxruntime.dylib");
    REQUIRE_FALSE(res.found);
    REQUIRE(res.source == ResourceSource::ExplicitOverride);
    REQUIRE_FALSE(res.error_msg.empty());
  }

  SECTION("Python wheel ORT path is rejected") {
    auto res = locator.locate_onnxruntime_library(
        "/tmp/.venv/lib/python3.12/site-packages/onnxruntime/capi/libonnxruntime.1.28.0.dylib");
    REQUIRE_FALSE(res.found);
    REQUIRE(res.error_msg.find("Python wheel") != std::string::npos);
  }
}
