#include <catch2/catch_test_macros.hpp>

#include "sublift/changepoint.hpp"
#include "sublift/image.hpp"
#include "sublift/signature.hpp"
#include "sublift/test_support.hpp"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef SUBLIFT_PARITY_GOLDEN_CHANGEPOINT
#error "SUBLIFT_PARITY_GOLDEN_CHANGEPOINT must be defined by CMake"
#endif
#ifndef SUBLIFT_PARITY_FIXTURE_CHANGEPOINT
#error "SUBLIFT_PARITY_FIXTURE_CHANGEPOINT must be defined by CMake"
#endif

namespace {

std::string event_type_name(sublift::EventType t) {
  switch (t) {
    case sublift::EventType::In:
      return "IN";
    case sublift::EventType::Out:
      return "OUT";
    case sublift::EventType::Change:
      return "CHANGE";
  }
  return "UNKNOWN";
}

sublift::ImageBuffer load_crop_rgb(
    const std::filesystem::path& fixtures_dir,
    const sublift::test_support::ChangepointCropGolden& c) {
  if (c.width <= 0 || c.height <= 0) {
    throw std::runtime_error("crop " + c.name + ": bad geometry");
  }
  const auto path = fixtures_dir / c.asset;
  std::ifstream in(path, std::ios::binary);
  if (!in) {
    throw std::runtime_error("cannot open crop: " + path.string());
  }
  const std::size_t nbytes =
      static_cast<std::size_t>(c.width) * static_cast<std::size_t>(c.height) * 3u;
  in.seekg(0, std::ios::end);
  if (static_cast<std::uint64_t>(in.tellg()) != nbytes) {
    throw std::runtime_error("crop size mismatch: " + path.string());
  }
  in.seekg(0, std::ios::beg);
  std::vector<std::uint8_t> buf(nbytes);
  in.read(reinterpret_cast<char*>(buf.data()),
          static_cast<std::streamsize>(nbytes));
  const auto digest = sublift::test_support::sha256_prefixed(buf);
  if (digest != c.input_asset_sha256) {
    throw std::runtime_error("crop sha mismatch: " + c.name);
  }
  sublift::ImageBuffer image(c.width, c.height, sublift::PixelFormat::RGB24);
  std::copy(buf.begin(), buf.end(), image.data());
  return image;
}

}  // namespace

TEST_CASE("changepoint parity vs frozen golden", "[parity][changepoint]") {
  const auto golden_path =
      std::filesystem::path{SUBLIFT_PARITY_GOLDEN_CHANGEPOINT};
  const auto fixtures_dir =
      std::filesystem::path{SUBLIFT_PARITY_FIXTURE_CHANGEPOINT};
  const auto golden =
      sublift::test_support::load_changepoint_golden(golden_path);

  REQUIRE(golden.oracle.golden_schema_version == 1);
  REQUIRE_FALSE(golden.oracle.oracle_commit.empty());
  REQUIRE_FALSE(golden.scenarios.empty());

  std::unordered_map<std::string, sublift::ImageBuffer> crops;
  for (const auto& c : golden.crops) {
    crops.emplace(c.name, load_crop_rgb(fixtures_dir, c));
  }

  for (const auto& sc : golden.scenarios) {
    sublift::ChangePointDetector det(sc.change_point_config);
    std::vector<sublift::test_support::ChangepointEventGolden> got;
    for (const auto& fr : sc.frames) {
      sublift::FrameSignature sig{fr.timestamp_ms, fr.foreground_ratio,
                                  fr.dhash};
      std::optional<sublift::ImageView> crop_holder;
      const sublift::ImageView* crop_view = nullptr;
      if (fr.crop_name.has_value()) {
        auto it = crops.find(*fr.crop_name);
        REQUIRE(it != crops.end());
        crop_holder = it->second.view();
        crop_view = &*crop_holder;
      }
      if (auto ev = det.process(sig, crop_view)) {
        got.push_back(sublift::test_support::ChangepointEventGolden{
            .event_type = event_type_name(ev->event_type),
            .timestamp_ms = ev->timestamp_ms,
            .prev_end_ms = ev->prev_end_ms,
        });
      }
    }
    const auto diff = sublift::test_support::diff_changepoint_events(
        got, sc.events, sc.name, &golden.oracle);
    INFO("scenario: " << sc.name << "\n"
                      << (diff.has_value() ? *diff : std::string{"ok"}));
    REQUIRE_FALSE(diff.has_value());
  }
}
