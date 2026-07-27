#include <catch2/catch_test_macros.hpp>

#include "sublift/config.hpp"

using namespace sublift;

TEST_CASE("default Config full field snapshot", "[config]") {
  // Keep in sync with src/sublift/config.py Config (16 top-level fields).
  const Config cfg{};

  REQUIRE(cfg.sample_fps == 5.0);
  REQUIRE(cfg.region_bottom_ratio == 0.3);
  REQUIRE(cfg.confidence_threshold == 0.5);
  REQUIRE(cfg.merge_gap_ms == 1000);
  REQUIRE(cfg.min_duration_ms == 500);
  REQUIRE(cfg.ocr_anchor_delay_frames == 2);
  REQUIRE(cfg.drop_empty_text == false);
  REQUIRE_FALSE(cfg.subtitle_profile.has_value());
  REQUIRE(cfg.subtitle_script == "auto");
  REQUIRE(cfg.enable_line_select == true);
  REQUIRE(cfg.low_conf_threshold == 0.28);
  REQUIRE(cfg.ocr_consensus_frames == 4);
  REQUIRE(cfg.line_select_min_score == 0.28);
  REQUIRE(cfg.line_select_min_script == 0.12);

  REQUIRE(cfg.signature.block_size_ratio == 0.08);
  REQUIRE(cfg.signature.adaptive_c == 12);
  REQUIRE(cfg.signature.hash_size == 8);

  REQUIRE(cfg.change_point.presence_threshold == 0.01);
  REQUIRE(cfg.change_point.hysteresis_frames == 1);
  REQUIRE(cfg.change_point.change_threshold == 10);
  REQUIRE(cfg.change_point.enable_ssim_verify == false);
  REQUIRE(cfg.change_point.ssim_threshold == 0.95);
  REQUIRE(cfg.change_point.ssim_window_size == 7);
  REQUIRE(cfg.change_point.enable_ssim_patrol == true);
  REQUIRE(cfg.change_point.ssim_patrol_interval == 3);
  REQUIRE(cfg.change_point.ssim_patrol_threshold == 0.92);
  REQUIRE(cfg.change_point.ssim_patrol_use_mask == true);
}

TEST_CASE("kDefaultConfig matches default-constructed Config", "[config]") {
  const Config a{};
  const Config& b = kDefaultConfig;
  REQUIRE(a == b);
}

TEST_CASE("designated init overrides one field", "[config]") {
  const Config cfg{.sample_fps = 10.0};
  REQUIRE(cfg.sample_fps == 10.0);
  REQUIRE(cfg.merge_gap_ms == 1000);
  REQUIRE(cfg.signature.hash_size == 8);
}
