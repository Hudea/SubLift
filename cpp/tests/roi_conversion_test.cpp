#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <fstream>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "sublift/server/job_manager.hpp"

#ifndef SUBLIFT_CONTRACT_ROI
#error "SUBLIFT_CONTRACT_ROI must be defined by CMake"
#endif

TEST_CASE("normalized ROI converts to contract pixel boxes",
          "[roi][contracts]") {
  std::ifstream in(SUBLIFT_CONTRACT_ROI);
  REQUIRE(in.is_open());
  nlohmann::json root;
  in >> root;
  REQUIRE(root.is_array());
  REQUIRE_FALSE(root.empty());
  for (const auto& item : root) {
    const auto& source = item.at("source");
    const auto& normalized = item.at("normalized");
    const auto& expected = item.at("expected_pixels");
    const sublift::server::RegionBox box{
        normalized.at("x").get<double>(),
        normalized.at("y").get<double>(),
        normalized.at("width").get<double>(),
        normalized.at("height").get<double>(),
    };
    const auto pixels =
        box.to_pixel_box(source.at("width").get<std::int32_t>(),
                         source.at("height").get<std::int32_t>());
    CHECK(pixels.x == expected.at("x").get<std::int32_t>());
    CHECK(pixels.y == expected.at("y").get<std::int32_t>());
    CHECK(pixels.width == expected.at("width").get<std::int32_t>());
    CHECK(pixels.height == expected.at("height").get<std::int32_t>());
  }
}

TEST_CASE("normalized ROI rejects empty frames", "[roi][contracts]") {
  const sublift::server::RegionBox box{};
  CHECK_THROWS_AS(box.to_pixel_box(0, 1080), std::invalid_argument);
  CHECK_THROWS_AS(box.to_pixel_box(1920, 0), std::invalid_argument);
}
