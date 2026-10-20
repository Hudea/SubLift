#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <string>

#include "sublift/adapters/paddle.hpp"
#include "sublift/models/native_manifest.hpp"

namespace fs = std::filesystem;

namespace {

/// Unique scratch directory name so parallel test processes do not collide.
std::string unique_suffix() {
  return std::to_string(
      static_cast<long long>(std::chrono::steady_clock::now().time_since_epoch().count()));
}

bool paddle_ready() { return sublift::is_paddle_available(); }

/// Copy the installed model bundle into a scratch dir so the test can mutate
/// mtimes without touching the user cache or the repo.
fs::path copy_bundle_to_scratch(const std::string& tag) {
  const fs::path source = sublift::models::default_native_model_dir();
  const fs::path scratch = fs::temp_directory_path() / ("sublift-probe-" + tag + "-" + unique_suffix());
  fs::create_directories(scratch);
  for (const auto& entry : fs::directory_iterator(source)) {
    if (entry.is_regular_file()) {
      fs::copy_file(entry.path(), scratch / entry.path().filename(),
                    fs::copy_options::overwrite_existing);
    }
  }
  return scratch;
}

struct ScratchCleanup {
  fs::path path;
  ~ScratchCleanup() {
    std::error_code ec;
    fs::remove_all(path, ec);
  }
};

}  // namespace

TEST_CASE("Paddle capability probe cache reuses a successful probe (O8)", "[paddle][probe]") {
  if (!paddle_ready()) {
    SKIP("Paddle model bundle is not installed on this machine");
  }
  sublift::reset_paddle_probe_cache_for_testing();

  const auto first = sublift::PaddleOcrEngine::probe_capabilities();
  REQUIRE(first.available);
  {
    const auto stats = sublift::paddle_probe_cache_stats();
    REQUIRE(stats.misses == 1);
    REQUIRE(stats.hits == 0);
    REQUIRE(stats.stores == 1);
  }

  const auto second = sublift::PaddleOcrEngine::probe_capabilities();
  REQUIRE(second.available);
  REQUIRE(second.model_root == first.model_root);
  REQUIRE(second.detail == first.detail);
  const auto stats = sublift::paddle_probe_cache_stats();
  REQUIRE(stats.hits == 1);
  REQUIRE(stats.misses == 1);
  REQUIRE(stats.stores == 1);
}

TEST_CASE("Paddle capability probe cache invalidates on bundle fingerprint change (O8)",
          "[paddle][probe]") {
  if (!paddle_ready()) {
    SKIP("Paddle model bundle is not installed on this machine");
  }
  const fs::path scratch = copy_bundle_to_scratch("fingerprint");
  const ScratchCleanup cleanup{scratch};

  sublift::reset_paddle_probe_cache_for_testing();
  REQUIRE(sublift::PaddleOcrEngine::probe_capabilities(scratch.string()).available);
  REQUIRE(sublift::PaddleOcrEngine::probe_capabilities(scratch.string()).available);
  {
    const auto stats = sublift::paddle_probe_cache_stats();
    REQUIRE(stats.misses == 1);
    REQUIRE(stats.hits == 1);
    REQUIRE(stats.stores == 1);
  }

  // Same bytes, new mtime: the fingerprint must change and force a re-probe.
  fs::path touched;
  for (const auto& entry : fs::directory_iterator(scratch)) {
    if (entry.is_regular_file() && entry.path().extension() == ".onnx" &&
        entry.path().filename().string().find("det") != std::string::npos) {
      touched = entry.path();
      break;
    }
  }
  REQUIRE_FALSE(touched.empty());
  fs::last_write_time(touched, fs::last_write_time(touched) + std::chrono::seconds(5));

  REQUIRE(sublift::PaddleOcrEngine::probe_capabilities(scratch.string()).available);
  const auto stats = sublift::paddle_probe_cache_stats();
  REQUIRE(stats.misses == 2);
  REQUIRE(stats.hits == 1);
  REQUIRE(stats.stores == 2);
}

TEST_CASE("Paddle capability probe cache never serves stale entries (O8)", "[paddle][probe]") {
  if (!paddle_ready()) {
    SKIP("Paddle model bundle is not installed on this machine");
  }
  const fs::path missing =
      fs::temp_directory_path() / ("sublift-probe-missing-" + unique_suffix());
  REQUIRE_FALSE(fs::exists(missing));

  sublift::reset_paddle_probe_cache_for_testing();

  // Prime the cache with the real bundle.
  REQUIRE(sublift::PaddleOcrEngine::probe_capabilities().available);
  // A different (missing) root must not reuse the cached entry.
  const auto failed = sublift::PaddleOcrEngine::probe_capabilities(missing.string());
  REQUIRE_FALSE(failed.available);
  REQUIRE_FALSE(failed.detail.empty());
  {
    const auto stats = sublift::paddle_probe_cache_stats();
    REQUIRE(stats.misses == 2);
    REQUIRE(stats.stores == 1);  // failures are never cached
  }

  // The failure neither evicted nor replaced the good entry.
  REQUIRE(sublift::PaddleOcrEngine::probe_capabilities().available);
  {
    const auto stats = sublift::paddle_probe_cache_stats();
    REQUIRE(stats.hits == 1);
    REQUIRE(stats.misses == 2);
  }

  // No negative caching: the same failure re-probes every time.
  REQUIRE_FALSE(sublift::PaddleOcrEngine::probe_capabilities(missing.string()).available);
  const auto stats = sublift::paddle_probe_cache_stats();
  REQUIRE(stats.misses == 3);
  REQUIRE(stats.stores == 1);
}
