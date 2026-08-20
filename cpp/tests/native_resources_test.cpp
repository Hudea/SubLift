#include <catch2/catch_test_macros.hpp>

#include <filesystem>
#include <fstream>
#include <string_view>
#include <unistd.h>

#include "sublift/hash.hpp"
#include "sublift/models/native_manifest.hpp"
#include "sublift/models/resource_installer.hpp"
#include "sublift/models/resource_locator.hpp"

using sublift::models::InstallOptions;
using sublift::models::NativeResourceManifest;
using sublift::models::ResourceFileSpec;

namespace {

std::filesystem::path unique_dir(const std::string& name) {
  auto dir = std::filesystem::temp_directory_path() /
             ("sublift-native-resources-" + name + "-" + std::to_string(::getpid()));
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  return dir;
}

void write_text(const std::filesystem::path& path, std::string_view text) {
  std::ofstream out(path, std::ios::binary);
  out << text;
}

}  // namespace

TEST_CASE("Native resource manifest loads committed contract", "[models][native_resources]") {
  auto loaded = sublift::models::load_default_native_manifest();
  REQUIRE(loaded.ok);
  REQUIRE(loaded.manifest.schema_version == 1);
  REQUIRE(loaded.manifest.kind == "native-resources");
  REQUIRE(loaded.manifest.paddle.id == "ppocrv6-small");
  REQUIRE(loaded.manifest.paddle.files.size() == 4);
  REQUIRE_FALSE(loaded.manifest.onnxruntime.empty());
}

TEST_CASE("Python packaged ORT paths are not product sources", "[models][native_resources]") {
  REQUIRE(sublift::models::is_python_packaged_path(
      "/opt/app/.venv/lib/python3.12/site-packages/onnxruntime/capi/libonnxruntime.1.28.0.dylib"));
  REQUIRE_FALSE(sublift::models::is_python_packaged_path(
      "/opt/homebrew/opt/onnxruntime/lib/libonnxruntime.1.28.0.dylib"));
}

TEST_CASE("Atomic paddle install verifies SHA and does not publish junk",
          "[models][native_resources][install]") {
  const std::string payload = "native-resource-fixture\n";
  const auto digest = sublift::sha256_hex(
      reinterpret_cast<const std::uint8_t*>(payload.data()), payload.size());

  NativeResourceManifest manifest;
  manifest.schema_version = 1;
  manifest.kind = "native-resources";
  manifest.paddle.id = "fixture";
  manifest.paddle.version = "test";
  ResourceFileSpec spec;
  spec.role = "det";
  spec.filename = "toy.bin";
  spec.sha256 = digest;
  manifest.paddle.files.push_back(spec);

  const auto root = unique_dir("install");
  const auto source = root / "source";
  const auto dest = root / "dest";
  std::filesystem::create_directories(source);
  write_text(source / "toy.bin", payload);

  SECTION("copies verified bytes") {
    InstallOptions opts;
    opts.destination_dir = dest;
    opts.source_dirs = {source};
    opts.allow_network = false;
    auto result = sublift::models::install_paddle_bundle(manifest, opts);
    REQUIRE(result.ok);
    REQUIRE(std::filesystem::exists(dest / "toy.bin"));
    REQUIRE(sublift::sha256_file_hex(dest / "toy.bin") == digest);
    REQUIRE_FALSE(std::filesystem::exists(dest / ("toy.bin.partial." + std::to_string(::getpid()))));
  }

  SECTION("corrupt source is not published") {
    write_text(source / "toy.bin", "not-the-bytes");
    InstallOptions opts;
    opts.destination_dir = dest;
    opts.source_dirs = {source};
    opts.allow_network = false;
    auto result = sublift::models::install_paddle_bundle(manifest, opts);
    REQUIRE_FALSE(result.ok);
    REQUIRE_FALSE(std::filesystem::exists(dest / "toy.bin"));
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(dest, ec)) {
      REQUIRE(entry.path().filename().string().find(".partial.") == std::string::npos);
    }
  }

  SECTION("existing junk dest is not treated as available until replaced") {
    std::filesystem::create_directories(dest);
    write_text(dest / "toy.bin", "junk");
    REQUIRE(sublift::sha256_file_hex(dest / "toy.bin") != digest);
    write_text(source / "toy.bin", payload);
    InstallOptions opts;
    opts.destination_dir = dest;
    opts.source_dirs = {source};
    opts.allow_network = false;
    auto result = sublift::models::install_paddle_bundle(manifest, opts);
    REQUIRE(result.ok);
    REQUIRE(sublift::sha256_file_hex(dest / "toy.bin") == digest);
  }

  std::filesystem::remove_all(root);
}

TEST_CASE("Dummy model files fail digest-gated probe", "[models][native_resources]") {
  const auto dir = unique_dir("dummy-probe");
  const auto expected =
      sublift::models::get_expected_model_paths(dir, sublift::models::ModelType::Small);
  write_text(expected.det_path, "x");
  write_text(expected.cls_path, "x");
  write_text(expected.rec_path, "x");
  write_text(expected.keys_path, "x");

  sublift::models::ResourceLocator locator;
  auto exists_only = locator.probe_model_bundle(dir.string(), sublift::models::ModelType::Small, false);
  REQUIRE(exists_only.found);

  auto product = locator.probe_model_bundle(dir.string(), sublift::models::ModelType::Small, true);
  REQUIRE_FALSE(product.found);
  REQUIRE(product.error_msg.find("SHA-256") != std::string::npos);

  std::filesystem::remove_all(dir);
}

TEST_CASE("SHA-matching cache is a usable product bundle", "[models][native_resources]") {
  const auto legacy = sublift::models::default_legacy_model_dir();
  sublift::models::ResourceLocator locator;
  auto res = locator.probe_model_bundle("", sublift::models::ModelType::Small, true);
  if (!res.found) {
    SKIP("pinned PP-OCRv6-small files are not installed on this machine");
  }
  REQUIRE(std::filesystem::exists(res.value.det_path));
  auto digest = sublift::sha256_file_hex(res.value.det_path);
  REQUIRE(digest.has_value());
  REQUIRE(*digest == "090f04abcd9d9a7498bc4ebf677e4cb9bdce1fe4197ddb7e529f1ef44e1ff94f");
  (void)legacy;
}
