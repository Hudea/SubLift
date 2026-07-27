#include <catch2/catch_test_macros.hpp>

#include "sublift/image.hpp"
#include "sublift/signature.hpp"
#include "sublift/test_support.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#ifndef SUBLIFT_PARITY_GOLDEN_SIGNATURE
#error "SUBLIFT_PARITY_GOLDEN_SIGNATURE must be defined by CMake"
#endif
#ifndef SUBLIFT_PARITY_FIXTURE_SIGNATURE
#error "SUBLIFT_PARITY_FIXTURE_SIGNATURE must be defined by CMake"
#endif

namespace {

/// Map fixture pixel_semantics to the PixelFormat the C++ candidate must tag
/// the loaded bytes with. Crucially, bgr_quirk maps to RGB24 so to_gray feeds
/// BGR-ordered bytes through COLOR_RGB2GRAY (the quirk), NOT BGR2GRAY.
sublift::PixelFormat semantics_to_format(const std::string& s) {
  if (s == "rgb24" || s == "bgr24_as_rgb_gray_quirk") {
    return sublift::PixelFormat::RGB24;
  }
  if (s == "gray8") {
    return sublift::PixelFormat::Gray8;
  }
  throw std::runtime_error("unknown pixel_semantics: " + s);
}

[[nodiscard]] std::size_t checked_nbytes(std::int32_t width, std::int32_t height,
                                         int bpp, const std::string& name) {
  if (width <= 0 || height <= 0) {
    throw std::runtime_error("fixture " + name + ": width/height must be > 0");
  }
  if (bpp <= 0) {
    throw std::runtime_error("fixture " + name + ": invalid bytes_per_pixel");
  }
  const auto w = static_cast<std::uint64_t>(width);
  const auto h = static_cast<std::uint64_t>(height);
  const auto b = static_cast<std::uint64_t>(bpp);
  // Checked multiply: w * h * bpp must fit in size_t.
  if (w > std::numeric_limits<std::uint64_t>::max() / h) {
    throw std::runtime_error("fixture " + name + ": width*height overflow");
  }
  const std::uint64_t pixels = w * h;
  if (pixels > std::numeric_limits<std::uint64_t>::max() / b) {
    throw std::runtime_error("fixture " + name + ": pixel byte count overflow");
  }
  const std::uint64_t nbytes64 = pixels * b;
  if (nbytes64 > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    throw std::runtime_error("fixture " + name + ": size exceeds size_t");
  }
  return static_cast<std::size_t>(nbytes64);
}

sublift::ImageBuffer load_fixture(
    const std::filesystem::path& dir,
    const sublift::test_support::SignatureFixtureGolden& f) {
  if (f.input_asset_sha256.empty()) {
    throw std::runtime_error("fixture " + f.name + ": empty input_asset_sha256");
  }

  const auto path = dir / f.asset;
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("cannot open fixture: " + path.string());
  }
  const auto fmt = semantics_to_format(f.pixel_semantics);
  const int bpp = sublift::bytes_per_pixel(fmt);
  const std::size_t nbytes = checked_nbytes(f.width, f.height, bpp, f.name);

  // Exact file size: reject short and oversize assets (parity integrity).
  in.seekg(0, std::ios::end);
  const auto file_size = static_cast<std::uint64_t>(in.tellg());
  in.seekg(0, std::ios::beg);
  if (file_size != static_cast<std::uint64_t>(nbytes)) {
    throw std::runtime_error(
        "fixture " + f.name + ": size mismatch path=" + path.string() +
        " expected=" + std::to_string(nbytes) +
        " got=" + std::to_string(file_size));
  }

  std::vector<std::uint8_t> buf(nbytes);
  in.read(reinterpret_cast<char*>(buf.data()),
          static_cast<std::streamsize>(nbytes));
  if (static_cast<std::size_t>(in.gcount()) != nbytes) {
    throw std::runtime_error("short read on fixture: " + path.string());
  }

  const auto digest = sublift::test_support::sha256_prefixed(buf);
  if (digest != f.input_asset_sha256) {
    throw std::runtime_error(
        "fixture " + f.name + ": input_asset_sha256 mismatch path=" +
        path.string() + " expected=" + f.input_asset_sha256 +
        " got=" + digest);
  }

  sublift::ImageBuffer image(f.width, f.height, fmt);
  std::copy(buf.begin(), buf.end(), image.data());
  return image;
}

}  // namespace

TEST_CASE("signature parity vs frozen golden", "[parity][signature]") {
  const auto golden_path = std::filesystem::path{SUBLIFT_PARITY_GOLDEN_SIGNATURE};
  const auto fixtures_dir = std::filesystem::path{SUBLIFT_PARITY_FIXTURE_SIGNATURE};
  const auto golden = sublift::test_support::load_signature_golden(golden_path);

  REQUIRE(golden.oracle.golden_schema_version == 1);
  REQUIRE_FALSE(golden.oracle.oracle_commit.empty());
  REQUIRE_FALSE(golden.fixtures.empty());

  for (const auto& f : golden.fixtures) {
    const auto image = load_fixture(fixtures_dir, f);
    const auto sig = sublift::compute_signature(image.view(), f.timestamp_ms,
                                                golden.signature_config);
    const auto diff =
        sublift::test_support::diff_signature(sig, f, &golden.oracle);
    INFO("fixture: " << f.name << "\n"
                     << (diff.has_value() ? *diff : std::string{"ok"}));
    REQUIRE_FALSE(diff.has_value());
  }
}

TEST_CASE("signature fixture loader rejects bad geometry", "[parity][signature]") {
  sublift::test_support::SignatureFixtureGolden bad{
      .name = "bad_dims",
      .pixel_semantics = "rgb24",
      .asset = "empty.rgb",
      .input_asset_sha256 = "sha256:deadbeef",
      .width = 0,
      .height = 64,
      .timestamp_ms = 0,
      .fg_ratio = 0.0,
      .dhash = 0,
  };
  const auto fixtures_dir = std::filesystem::path{SUBLIFT_PARITY_FIXTURE_SIGNATURE};
  REQUIRE_THROWS_AS(load_fixture(fixtures_dir, bad), std::runtime_error);

  bad.width = 128;
  bad.height = -1;
  REQUIRE_THROWS_AS(load_fixture(fixtures_dir, bad), std::runtime_error);
}
