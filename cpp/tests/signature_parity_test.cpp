#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <golden_loader.hpp>
#include <sublift/core/image_view.hpp>
#include <sublift/features/dhash.hpp>
#include <sublift/features/mask.hpp>
#include <sublift/features/visual_analysis.hpp>

namespace {

std::filesystem::path find_parity_dir() {
#ifdef SUBLIFT_PROJECT_SOURCE_DIR
  std::filesystem::path macro_cand = std::filesystem::path(SUBLIFT_PROJECT_SOURCE_DIR) / "benchmark/parity";
  if (std::filesystem::exists(macro_cand / "goldens/signature/signature.v1.json")) {
    return macro_cand;
  }
#endif
  std::vector<std::filesystem::path> candidates = {
      "benchmark/parity",
      "../benchmark/parity",
      "../../benchmark/parity",
      "../../../benchmark/parity",
  };
  for (const auto& p : candidates) {
    if (std::filesystem::exists(p / "goldens/signature/signature.v1.json")) {
      return p;
    }
  }
  return "";
}

} // namespace

TEST_CASE("Milestone 2 Golden Parity Verification", "[features][parity]") {
  const std::filesystem::path parity_dir = find_parity_dir();
  REQUIRE_FALSE(parity_dir.empty());

  const std::filesystem::path golden_json_path = parity_dir / "goldens/signature/signature.v1.json";
  auto golden = sublift::test::load_golden_json(golden_json_path);
  REQUIRE(golden.is_object());

  const auto& sig_cfg = golden["signature_config"];
  const double block_size_ratio = sig_cfg["block_size_ratio"].get<double>();
  const double adaptive_c = sig_cfg["adaptive_c"].get<double>();
  const int hash_size = sig_cfg["hash_size"].get<int>();

  REQUIRE(block_size_ratio == 0.08);
  REQUIRE(adaptive_c == 12.0);
  REQUIRE(hash_size == 8);

  const sublift::features::MaskConfig mask_config{
      .block_size_ratio = block_size_ratio,
      .adaptive_c = adaptive_c,
      .morph_kernel_width = 2,
      .morph_kernel_height = 2,
  };

  const auto& fixtures = golden["fixtures"];
  REQUIRE(fixtures.is_array());
  REQUIRE(fixtures.size() == 5);

  for (const auto& fix : fixtures) {
    const std::string name = fix["name"].get<std::string>();
    const std::string semantics = fix["pixel_semantics"].get<std::string>();
    const std::string asset = fix["asset"].get<std::string>();
    const int width = fix["width"].get<int>();
    const int height = fix["height"].get<int>();
    const std::int64_t timestamp_ms = fix["timestamp_ms"].get<std::int64_t>();
    const double expected_fg = fix["fg_ratio"].get<double>();
    const std::uint64_t expected_dhash = sublift::test::get_uint64(fix["dhash"]);

    DYNAMIC_SECTION("Golden fixture: " << name) {
      const std::filesystem::path asset_path = parity_dir / "fixtures/signature" / asset;
      std::ifstream asset_file(asset_path, std::ios::binary);
      REQUIRE(asset_file.is_open());

      std::vector<std::uint8_t> raw((std::istreambuf_iterator<char>(asset_file)),
                                     std::istreambuf_iterator<char>());

      const bool is_gray = (semantics == "gray8");
      const std::size_t expected_bytes = static_cast<std::size_t>(width * height * (is_gray ? 1 : 3));
      REQUIRE(raw.size() == expected_bytes);

      // DEV-001 contract: bgr_quirk is correctly interpreted as RGB24 with standard RGB weights
      const auto format = is_gray ? sublift::PixelFormat::Gray8 : sublift::PixelFormat::RGB24;
      const int stride = width * (is_gray ? 1 : 3);
      const sublift::ImageView view(raw.data(), width, height, stride, format);

      auto analysis = sublift::features::compute_visual_analysis(view, timestamp_ms, mask_config);

      REQUIRE(analysis.signature.timestamp_ms == timestamp_ms);

      // Verify 100% bit-exact dHash match (Hamming distance == 0)
      const int dhash_hamming = sublift::features::hamming_distance(analysis.signature.dhash, expected_dhash);
      INFO("Fixture: " << name << ", computed dhash: " << analysis.signature.dhash
                       << ", expected: " << expected_dhash << ", hamming: " << dhash_hamming);
      REQUIRE(dhash_hamming == 0);
      REQUIRE(analysis.signature.dhash == expected_dhash);

      // Verify foreground ratio difference <= 1e-5
      const double fg_diff = std::abs(analysis.signature.foreground_ratio - expected_fg);
      INFO("Fixture: " << name << ", computed fg: " << analysis.signature.foreground_ratio
                       << ", expected: " << expected_fg << ", diff: " << fg_diff);
      REQUIRE(fg_diff <= 1e-5);
    }
  }
}
