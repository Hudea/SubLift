#include <cstdint>
#include <cstring>
#include <tuple>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <sublift/core/color.hpp>
#include <sublift/core/image_buffer.hpp>
#include <sublift/core/image_view.hpp>
#include <sublift/features/mask.hpp>
#include <sublift/features/signature.hpp>
#include <sublift/timing/changepoint.hpp>
#include <sublift/timing/frame_analysis.hpp>
#include <sublift/timing/segment.hpp>
#include <sublift/timing/timeline.hpp>
#include <sublift/timing/timing_config.hpp>

namespace {

sublift::ImageBuffer make_test_image(int width, int height, sublift::PixelFormat format, uint8_t fill_val) {
  sublift::ImageBuffer buf(width, height, format);
  for (int y = 0; y < height; ++y) {
    uint8_t* row = buf.row(y);
    int row_bytes = width * (format == sublift::PixelFormat::RGB24 ? 3 : 1);
    std::memset(row, fill_val, row_bytes);
  }
  return buf;
}

sublift::ImageBuffer make_wide_text_image(int width, int height, uint8_t bg_val, uint8_t fg_val,
                                         bool include_left, bool include_center, bool include_right) {
  sublift::ImageBuffer buf(width, height, sublift::PixelFormat::RGB24);
  for (int y = 0; y < height; ++y) {
    uint8_t* row = buf.row(y);
    std::memset(row, bg_val, width * 3);
  }

  // Sector boundaries: left < 0.25*w, center in [0.25*w, 0.75*w), right >= 0.75*w
  const int l_cut = static_cast<int>(0.25 * width);
  const int r_cut = static_cast<int>(0.75 * width);

  auto draw_box = [&](int x0, int x1, int y0, int y1) {
    for (int y = y0; y < y1 && y < height; ++y) {
      uint8_t* row = buf.row(y);
      for (int x = x0; x < x1 && x < width; ++x) {
        row[x * 3 + 0] = fg_val;
        row[x * 3 + 1] = fg_val;
        row[x * 3 + 2] = fg_val;
      }
    }
  };

  // Draw >= 40 foreground pixels in each enabled sector
  if (include_left) {
    draw_box(2, std::min(l_cut - 2, 22), 4, 16); // ~20 * 12 = 240 pixels
  }
  if (include_center) {
    int cx = width / 2;
    draw_box(cx - 10, cx + 10, 4, 16); // 20 * 12 = 240 pixels
  }
  if (include_right) {
    draw_box(r_cut + 2, std::min(width - 2, r_cut + 22), 4, 16); // 20 * 12 = 240 pixels
  }

  return buf;
}

sublift::ImageBuffer make_wide_text_mask(int width, int height,
                                         bool include_left, bool include_center, bool include_right) {
  sublift::ImageBuffer buf(width, height, sublift::PixelFormat::Gray8);
  for (int y = 0; y < height; ++y) {
    uint8_t* row = buf.row(y);
    std::memset(row, 0, width);
  }

  const int l_cut = static_cast<int>(0.25 * width);
  const int r_cut = static_cast<int>(0.75 * width);

  auto draw_box = [&](int x0, int x1, int y0, int y1) {
    for (int y = y0; y < y1 && y < height; ++y) {
      uint8_t* row = buf.row(y);
      for (int x = x0; x < x1 && x < width; ++x) {
        row[x] = 255;
      }
    }
  };

  if (include_left) {
    draw_box(2, std::min(l_cut - 2, 22), 4, 16);
  }
  if (include_center) {
    int cx = width / 2;
    draw_box(cx - 10, cx + 10, 4, 16);
  }
  if (include_right) {
    draw_box(r_cut + 2, std::min(width - 2, r_cut + 22), 4, 16);
  }

  return buf;
}

} // namespace

TEST_CASE("ChangePointDetector millisecond state transitions and backtracking (DEV-002, DEV-003)",
          "[timing][changepoint]") {
  sublift::timing::TimingConfig cfg{
      .hysteresis_ms = 200,
      .confirm_ms = 200,
      .patrol_interval_ms = 600,
      .change_threshold = 10,
      .presence_threshold = 0.05,
      .enable_ssim_patrol = false,
      .enable_ssim_verify = false,
  };

  sublift::timing::ChangePointDetector detector(cfg);
  REQUIRE(detector.current_state() == sublift::timing::State::Empty);

  SECTION("DEV-002: Subtitle onset at 0ms is preserved (not skipped as falsy)") {
    // Frame 1 at 0ms: presence starts
    auto ev = detector.process(sublift::features::FrameSignature{
        .timestamp_ms = 0,
        .foreground_ratio = 0.12,
        .dhash = 0x1111222233334444ULL,
    });
    REQUIRE_FALSE(ev.has_value());
    REQUIRE(detector.current_state() == sublift::timing::State::Empty);

    // Frame 2 at 100ms: presence persists (< 200ms hysteresis)
    ev = detector.process(sublift::features::FrameSignature{
        .timestamp_ms = 100,
        .foreground_ratio = 0.12,
        .dhash = 0x1111222233334444ULL,
    });
    REQUIRE_FALSE(ev.has_value());

    // Frame 3 at 200ms: elapsed 200ms >= hysteresis_ms (200ms) -> Confirmed IN!
    ev = detector.process(sublift::features::FrameSignature{
        .timestamp_ms = 200,
        .foreground_ratio = 0.12,
        .dhash = 0x1111222233334444ULL,
    });
    REQUIRE(ev.has_value());
    REQUIRE(ev->type == sublift::timing::EventType::In);
    REQUIRE(ev->timestamp_ms == 0); // Preserves exact 0ms start!
    REQUIRE_FALSE(ev->prev_end_ms.has_value());
    REQUIRE(detector.current_state() == sublift::timing::State::Stable);
  }

  SECTION("Full lifecycle: In -> Stable -> Change -> StablePrime -> Out -> Empty") {
    // 1. Enter Stable at 1000ms
    detector.process(sublift::features::FrameSignature{
        .timestamp_ms = 1000,
        .foreground_ratio = 0.20,
        .dhash = 0xAAAAAAAAAAAAAAAAULL,
    });
    auto ev_in = detector.process(sublift::features::FrameSignature{
        .timestamp_ms = 1250,
        .foreground_ratio = 0.20,
        .dhash = 0xAAAAAAAAAAAAAAAAULL,
    });
    REQUIRE(ev_in.has_value());
    REQUIRE(ev_in->type == sublift::timing::EventType::In);
    REQUIRE(ev_in->timestamp_ms == 1000);
    REQUIRE(detector.current_state() == sublift::timing::State::Stable);

    // 2. Steady frames in Stable
    auto ev_steady = detector.process(sublift::features::FrameSignature{
        .timestamp_ms = 1500,
        .foreground_ratio = 0.20,
        .dhash = 0xAAAAAAAAAAAAAAAAULL,
    });
    REQUIRE_FALSE(ev_steady.has_value());
    REQUIRE(detector.current_state() == sublift::timing::State::Stable);

    // 3. Content Change begins at 2000ms (dHash distance >> change_threshold)
    auto ev_c1 = detector.process(sublift::features::FrameSignature{
        .timestamp_ms = 2000,
        .foreground_ratio = 0.20,
        .dhash = 0x5555555555555555ULL,
    });
    REQUIRE_FALSE(ev_c1.has_value());

    // Content Change confirmed at 2220ms (>= 200ms confirm passed)
    auto ev_c2 = detector.process(sublift::features::FrameSignature{
        .timestamp_ms = 2220,
        .foreground_ratio = 0.20,
        .dhash = 0x5555555555555555ULL,
    });
    REQUIRE(ev_c2.has_value());
    REQUIRE(ev_c2->type == sublift::timing::EventType::Change);
    REQUIRE(ev_c2->timestamp_ms == 2000);
    REQUIRE(ev_c2->prev_end_ms == 2000);
    REQUIRE(detector.current_state() == sublift::timing::State::StablePrime);

    // 4. Subtitle disappearance begins at 3000ms
    auto ev_o1 = detector.process(sublift::features::FrameSignature{
        .timestamp_ms = 3000,
        .foreground_ratio = 0.01, // below presence_threshold
        .dhash = 0x0ULL,
    });
    REQUIRE_FALSE(ev_o1.has_value());

    // Subtitle disappearance confirmed at 3250ms (>= 200ms hysteresis passed)
    auto ev_o2 = detector.process(sublift::features::FrameSignature{
        .timestamp_ms = 3250,
        .foreground_ratio = 0.01,
        .dhash = 0x0ULL,
    });
    REQUIRE(ev_o2.has_value());
    REQUIRE(ev_o2->type == sublift::timing::EventType::Out);
    REQUIRE(ev_o2->timestamp_ms == 3000);
    REQUIRE(detector.current_state() == sublift::timing::State::Empty);

    // 5. Detector reset
    detector.reset();
    REQUIRE(detector.current_state() == sublift::timing::State::Empty);
    REQUIRE_FALSE(detector.anchor_signature().has_value());
    REQUIRE_FALSE(detector.anchor_crop().has_value());
  }
}

TEST_CASE("ChangePointDetector jitter and transient disturbance suppression",
          "[timing][jitter]") {
  sublift::timing::TimingConfig cfg{
      .hysteresis_ms = 200,
      .confirm_ms = 200,
      .change_threshold = 10,
      .presence_threshold = 0.05,
      .enable_ssim_patrol = false,
      .enable_ssim_verify = false,
  };

  sublift::timing::ChangePointDetector detector(cfg);

  SECTION("Single-frame flash in Empty state is suppressed") {
    // Frame at 0ms: empty
    detector.process(sublift::features::FrameSignature{.timestamp_ms = 0, .foreground_ratio = 0.0, .dhash = 0});

    // Frame at 100ms: single flash frame (foreground present)
    auto ev_flash = detector.process(sublift::features::FrameSignature{
        .timestamp_ms = 100, .foreground_ratio = 0.15, .dhash = 0x1234});
    REQUIRE_FALSE(ev_flash.has_value());

    // Frame at 140ms: back to empty (flash ended before hysteresis_ms)
    auto ev_next = detector.process(sublift::features::FrameSignature{
        .timestamp_ms = 140, .foreground_ratio = 0.0, .dhash = 0});
    REQUIRE_FALSE(ev_next.has_value());
    REQUIRE(detector.current_state() == sublift::timing::State::Empty);

    // Frame at 400ms: remains empty
    auto ev_later = detector.process(sublift::features::FrameSignature{
        .timestamp_ms = 400, .foreground_ratio = 0.0, .dhash = 0});
    REQUIRE_FALSE(ev_later.has_value());
    REQUIRE(detector.current_state() == sublift::timing::State::Empty);
  }

  SECTION("Single-frame dHash disturbance in Stable state is suppressed") {
    // Establish Stable state
    detector.process(sublift::features::FrameSignature{
        .timestamp_ms = 100, .foreground_ratio = 0.15, .dhash = 0xAAAAAAAAULL});
    auto ev_in = detector.process(sublift::features::FrameSignature{
        .timestamp_ms = 350, .foreground_ratio = 0.15, .dhash = 0xAAAAAAAAULL});
    REQUIRE(ev_in.has_value());
    REQUIRE(detector.current_state() == sublift::timing::State::Stable);

    // Frame at 500ms: single-frame noise glitch in dHash
    auto ev_glitch = detector.process(sublift::features::FrameSignature{
        .timestamp_ms = 500, .foreground_ratio = 0.15, .dhash = 0x55555555ULL});
    REQUIRE_FALSE(ev_glitch.has_value());

    // Frame at 540ms: immediately reverts to original anchor dHash
    auto ev_revert = detector.process(sublift::features::FrameSignature{
        .timestamp_ms = 540, .foreground_ratio = 0.15, .dhash = 0xAAAAAAAAULL});
    REQUIRE_FALSE(ev_revert.has_value());
    REQUIRE_FALSE(detector.change_candidate().has_value()); // candidate reset!
    REQUIRE(detector.current_state() == sublift::timing::State::Stable);

    // Frame at 800ms: still stable
    auto ev_stable = detector.process(sublift::features::FrameSignature{
        .timestamp_ms = 800, .foreground_ratio = 0.15, .dhash = 0xAAAAAAAAULL});
    REQUIRE_FALSE(ev_stable.has_value());
    REQUIRE(detector.current_state() == sublift::timing::State::Stable);
  }

  SECTION("Single-frame dropout / flicker in Stable state is suppressed") {
    // Establish Stable state
    detector.process(sublift::features::FrameSignature{
        .timestamp_ms = 100, .foreground_ratio = 0.15, .dhash = 0xAAAAAAAAULL});
    detector.process(sublift::features::FrameSignature{
        .timestamp_ms = 350, .foreground_ratio = 0.15, .dhash = 0xAAAAAAAAULL});
    REQUIRE(detector.current_state() == sublift::timing::State::Stable);

    // Frame at 600ms: single dropout frame (e.g. occlusion or binarization jitter)
    auto ev_drop = detector.process(sublift::features::FrameSignature{
        .timestamp_ms = 600, .foreground_ratio = 0.0, .dhash = 0});
    REQUIRE_FALSE(ev_drop.has_value());

    // Frame at 640ms: subtitle presence restores
    auto ev_restore = detector.process(sublift::features::FrameSignature{
        .timestamp_ms = 640, .foreground_ratio = 0.15, .dhash = 0xAAAAAAAAULL});
    REQUIRE_FALSE(ev_restore.has_value());
    REQUIRE(detector.current_state() == sublift::timing::State::Stable);
  }
}

TEST_CASE("ChangePointDetector SSIM patrol mechanism", "[timing][patrol]") {
  sublift::timing::TimingConfig cfg{
      .hysteresis_ms = 200,
      .confirm_ms = 200,
      .patrol_interval_ms = 600,
      .change_threshold = 10,
      .presence_threshold = 0.05,
      .enable_ssim_patrol = true,
      .ssim_patrol_threshold = 0.90,
      .ssim_patrol_use_mask = false,
      .ssim_window_size = 7,
      .aspect_ratio_threshold = 6.0,
  };

  sublift::timing::ChangePointDetector detector(cfg);

  // Setup test crops: height 20, width 80 (aspect ratio 4.0 <= 6.0, not wide)
  auto crop_a = make_test_image(80, 20, sublift::PixelFormat::RGB24, 20);
  // Add some foreground structure to crop_a
  for (int y = 5; y < 15; ++y) {
    uint8_t* row = crop_a.row(y);
    for (int x = 10; x < 40; ++x) {
      row[x * 3 + 0] = 240;
      row[x * 3 + 1] = 240;
      row[x * 3 + 2] = 240;
    }
  }

  // crop_b has completely different foreground structure (different location),
  // but we can give them the SAME dHash signature to specifically test SSIM patrol!
  auto crop_b = make_test_image(80, 20, sublift::PixelFormat::RGB24, 20);
  for (int y = 5; y < 15; ++y) {
    uint8_t* row = crop_b.row(y);
    for (int x = 45; x < 75; ++x) {
      row[x * 3 + 0] = 240;
      row[x * 3 + 1] = 240;
      row[x * 3 + 2] = 240;
    }
  }

  const uint64_t dummy_dhash = 0x1234567890ABCDEFULL;

  // Frame 1 at 0ms: enter with crop_a
  detector.process(
      sublift::features::FrameSignature{.timestamp_ms = 0, .foreground_ratio = 0.20, .dhash = dummy_dhash},
      crop_a.view());
  // Frame 2 at 250ms: confirm IN with crop_a
  auto ev_in = detector.process(
      sublift::features::FrameSignature{.timestamp_ms = 250, .foreground_ratio = 0.20, .dhash = dummy_dhash},
      crop_a.view());
  REQUIRE(ev_in.has_value());
  REQUIRE(detector.current_state() == sublift::timing::State::Stable);

  // Frame 3 at 400ms: crop_a persists, steady frame
  auto ev_3 = detector.process(
      sublift::features::FrameSignature{.timestamp_ms = 400, .foreground_ratio = 0.20, .dhash = dummy_dhash},
      crop_a.view());
  REQUIRE_FALSE(ev_3.has_value());

  // Frame 4 at 650ms: elapsed time >= 600ms -> patrol triggers!
  // Mask SSIM between crop_a and crop_b is very low (< 0.90).
  // Patrol creates a change candidate!
  auto ev_4 = detector.process(
      sublift::features::FrameSignature{.timestamp_ms = 650, .foreground_ratio = 0.20, .dhash = dummy_dhash},
      crop_b.view());
  REQUIRE_FALSE(ev_4.has_value());
  REQUIRE(detector.change_candidate().has_value());
  REQUIRE(detector.change_candidate()->timestamp_ms == 650);

  // Frame 5 at 870ms: candidate sustained for >= confirm_ms (200ms) -> Confirms CHANGE!
  auto ev_5 = detector.process(
      sublift::features::FrameSignature{.timestamp_ms = 870, .foreground_ratio = 0.20, .dhash = dummy_dhash},
      crop_b.view());
  REQUIRE(ev_5.has_value());
  REQUIRE(ev_5->type == sublift::timing::EventType::Change);
  REQUIRE(ev_5->timestamp_ms == 650);
  REQUIRE(detector.current_state() == sublift::timing::State::StablePrime);
}

TEST_CASE("ChangePointDetector SSIM change verification and veto (enable_ssim_verify = true)",
          "[timing][ssim_verify]") {
  sublift::timing::TimingConfig cfg{
      .hysteresis_ms = 200,
      .confirm_ms = 200,
      .change_threshold = 10,
      .presence_threshold = 0.05,
      .enable_ssim_patrol = false,
      .enable_ssim_verify = true,
      .ssim_threshold = 0.95,
      .ssim_window_size = 7,
  };

  sublift::timing::ChangePointDetector detector(cfg);

  auto crop_base = make_test_image(80, 20, sublift::PixelFormat::RGB24, 30);
  for (int y = 5; y < 15; ++y) {
    uint8_t* row = crop_base.row(y);
    for (int x = 10; x < 50; ++x) {
      row[x * 3 + 0] = 230;
      row[x * 3 + 1] = 230;
      row[x * 3 + 2] = 230;
    }
  }

  // 1. Establish stable state
  detector.process(
      sublift::features::FrameSignature{.timestamp_ms = 0, .foreground_ratio = 0.25, .dhash = 0xAAAAAAAAULL},
      crop_base.view());
  auto ev_in = detector.process(
      sublift::features::FrameSignature{.timestamp_ms = 250, .foreground_ratio = 0.25, .dhash = 0xAAAAAAAAULL},
      crop_base.view());
  REQUIRE(ev_in.has_value());
  REQUIRE(detector.current_state() == sublift::timing::State::Stable);

  SECTION("Veto: dHash changes significantly, but crop image is identical (SSIM ~ 1.0 > 0.95)") {
    // Large dHash distance (e.g. hash perturbation/collision) but identical crop image
    auto ev_vetoed = detector.process(
        sublift::features::FrameSignature{.timestamp_ms = 500, .foreground_ratio = 0.25, .dhash = 0x55555555ULL},
        crop_base.view());
    REQUIRE_FALSE(ev_vetoed.has_value());
    REQUIRE_FALSE(detector.change_candidate().has_value()); // candidate vetoed and discarded!
    REQUIRE(detector.current_state() == sublift::timing::State::Stable);

    // Later frame: still stable, no transition
    auto ev_later = detector.process(
        sublift::features::FrameSignature{.timestamp_ms = 750, .foreground_ratio = 0.25, .dhash = 0x55555555ULL},
        crop_base.view());
    REQUIRE_FALSE(ev_later.has_value());
    REQUIRE(detector.current_state() == sublift::timing::State::Stable);
  }

  SECTION("Allow: dHash changes significantly and crop image is truly different (SSIM < 0.95)") {
    auto crop_different = make_test_image(80, 20, sublift::PixelFormat::RGB24, 30);
    // Draw different structure
    for (int y = 5; y < 15; ++y) {
      uint8_t* row = crop_different.row(y);
      for (int x = 40; x < 75; ++x) {
        row[x * 3 + 0] = 230;
        row[x * 3 + 1] = 230;
        row[x * 3 + 2] = 230;
      }
    }

    auto ev_cand = detector.process(
        sublift::features::FrameSignature{.timestamp_ms = 500, .foreground_ratio = 0.25, .dhash = 0x55555555ULL},
        crop_different.view());
    REQUIRE_FALSE(ev_cand.has_value());
    REQUIRE(detector.change_candidate().has_value());

    auto ev_confirmed = detector.process(
        sublift::features::FrameSignature{.timestamp_ms = 750, .foreground_ratio = 0.25, .dhash = 0x55555555ULL},
        crop_different.view());
    REQUIRE(ev_confirmed.has_value());
    REQUIRE(ev_confirmed->type == sublift::timing::EventType::Change);
    REQUIRE(ev_confirmed->timestamp_ms == 500);
  }
}

TEST_CASE("ChangePointDetector sector-aware glyph retention veto for wide subtitles",
          "[timing][glyph_veto]") {
  sublift::timing::TimingConfig cfg{
      .hysteresis_ms = 200,
      .confirm_ms = 200,
      .change_threshold = 10,
      .presence_threshold = 0.05,
      .enable_ssim_patrol = false,
      .aspect_ratio_threshold = 6.0,
  };

  sublift::timing::ChangePointDetector detector(cfg);

  // Wide subtitle: width 140, height 20 -> Aspect ratio 7.0 > 6.0
  const int w = 140;
  const int h = 20;

  // anchor has text glyphs in all 3 sectors, background = 0, text = 255
  auto anchor_crop = make_wide_text_image(w, h, 0, 255, true, true, true);
  auto anchor_mask = make_wide_text_mask(w, h, true, true, true);

  // 1. Establish stable anchor
  detector.process(
      sublift::features::FrameSignature{.timestamp_ms = 0, .foreground_ratio = 0.20, .dhash = 0x11111111ULL},
      anchor_crop.view(), anchor_mask.view());
  auto ev_in = detector.process(
      sublift::features::FrameSignature{.timestamp_ms = 250, .foreground_ratio = 0.20, .dhash = 0x11111111ULL},
      anchor_crop.view(), anchor_mask.view());
  REQUIRE(ev_in.has_value());
  REQUIRE(detector.current_state() == sublift::timing::State::Stable);

  SECTION("Veto: dHash changes due to moving background, but text glyphs are retained in all 3 sectors") {
    // Current frame: text glyphs are identical, but background changed (bg = 30)
    // and dHash changes significantly (distance >> 10)
    auto bg_changed_crop = make_wide_text_image(w, h, 30, 255, true, true, true);
    auto bg_changed_mask = make_wide_text_mask(w, h, true, true, true);

    auto ev = detector.process(
        sublift::features::FrameSignature{.timestamp_ms = 500, .foreground_ratio = 0.20, .dhash = 0xEEEEEEEEULL},
        bg_changed_crop.view(), bg_changed_mask.view());
    REQUIRE_FALSE(ev.has_value());
    REQUIRE_FALSE(detector.change_candidate().has_value()); // Glyph retention vetoed the change!
    REQUIRE(detector.current_state() == sublift::timing::State::Stable);

    // Persisting with same moving background does not trigger Change
    auto ev2 = detector.process(
        sublift::features::FrameSignature{.timestamp_ms = 750, .foreground_ratio = 0.20, .dhash = 0xEEEEEEEEULL},
        bg_changed_crop.view(), bg_changed_mask.view());
    REQUIRE_FALSE(ev2.has_value());
    REQUIRE(detector.current_state() == sublift::timing::State::Stable);
  }

  SECTION("Allow: right sector text disappears/changes -> sector overlap drops -> Change emitted") {
    // Current frame: left and center text preserved, but right sector is cleared!
    auto text_changed_crop = make_wide_text_image(w, h, 0, 255, true, true, false);
    auto text_changed_mask = make_wide_text_mask(w, h, true, true, false);

    auto ev_c1 = detector.process(
        sublift::features::FrameSignature{.timestamp_ms = 500, .foreground_ratio = 0.15, .dhash = 0xEEEEEEEEULL},
        text_changed_crop.view(), text_changed_mask.view());
    REQUIRE_FALSE(ev_c1.has_value());
    REQUIRE(detector.change_candidate().has_value()); // Allowed, sector_changed = true!

    auto ev_c2 = detector.process(
        sublift::features::FrameSignature{.timestamp_ms = 750, .foreground_ratio = 0.15, .dhash = 0xEEEEEEEEULL},
        text_changed_crop.view(), text_changed_mask.view());
    REQUIRE(ev_c2.has_value());
    REQUIRE(ev_c2->type == sublift::timing::EventType::Change);
    REQUIRE(ev_c2->timestamp_ms == 500);
  }
}

TEST_CASE("ChangePointDetector FrameEvalCache and precomputed mask injection",
          "[timing][cache]") {
  sublift::timing::TimingConfig cfg{
      .hysteresis_ms = 200,
      .confirm_ms = 200,
      .enable_ssim_patrol = true,
      .ssim_patrol_use_mask = true,
  };

  sublift::timing::ChangePointDetector detector(cfg);

  auto crop = make_test_image(60, 20, sublift::PixelFormat::RGB24, 10);
  auto mask = make_test_image(60, 20, sublift::PixelFormat::Gray8, 255);

  SECTION("Precomputed valid_mask injection does not trigger mask computation") {
    auto crop_v = crop.view();
    auto mask_v = mask.view();
    detector.process(
        sublift::features::FrameSignature{.timestamp_ms = 0, .foreground_ratio = 0.2, .dhash = 0x1234},
        crop_v, mask_v);
    REQUIRE(detector.mask_computation_count() == 0);

    detector.process(
        sublift::features::FrameSignature{.timestamp_ms = 250, .foreground_ratio = 0.2, .dhash = 0x1234},
        crop_v, mask_v);
    REQUIRE(detector.mask_computation_count() == 0);
  }

  SECTION("FrameEvalCache hits on repeated pointer evaluations") {
    auto mask1 = make_test_image(30, 20, sublift::PixelFormat::Gray8, 100);
    auto mask2 = make_test_image(30, 20, sublift::PixelFormat::Gray8, 200);

    double val1 = detector.eval_mask_ssim(mask1.view(), mask2.view());
    REQUIRE(val1 >= 0.0);
    REQUIRE(detector.eval_cache().ssim_eval_count == 1);
    REQUIRE(detector.eval_cache().ssim_cache_hits == 0);

    double val2 = detector.eval_mask_ssim(mask1.view(), mask2.view());
    REQUIRE(val2 == val1);
    REQUIRE(detector.eval_cache().ssim_cache_hits == 1);

    // Identical pointer evaluation
    double val_self = detector.eval_mask_ssim(mask1.view(), mask1.view());
    REQUIRE(val_self == 1.0);
    REQUIRE(detector.eval_cache().ssim_cache_hits == 2);
  }
}

TEST_CASE("TimelineBuilder delayed anchor locking and priority fallback",
          "[timing][timeline][anchor]") {
  SECTION("Delayed anchor locking: ocr_anchor_delay_ms = 80") {
    sublift::timing::TimingConfig cfg{
        .ocr_anchor_delay_ms = 80,
        .ocr_consensus_frames = 4,
        .pure_timing = false,
    };
    sublift::timing::TimelineBuilder builder(cfg);

    auto frame_0 = make_test_image(40, 20, sublift::PixelFormat::RGB24, 10);
    auto frame_1 = make_test_image(40, 20, sublift::PixelFormat::RGB24, 20);
    auto frame_2 = make_test_image(40, 20, sublift::PixelFormat::RGB24, 30);
    auto frame_3 = make_test_image(40, 20, sublift::PixelFormat::RGB24, 40);

    // Frame 0: IN event at 100ms
    builder.consume(
        sublift::timing::StateEvent{.type = sublift::timing::EventType::In, .timestamp_ms = 100},
        frame_0.view());

    // Frame 1: stable frame at 140ms (delay 40ms < 80ms)
    builder.on_stable_frame(frame_1.view(), 140);

    // Frame 2: stable frame at 180ms (delay 80ms >= 80ms, locks delayed anchor!)
    builder.on_stable_frame(frame_2.view(), 180);

    // Frame 3: stable frame at 220ms
    builder.on_stable_frame(frame_3.view(), 220);

    // OUT event at 300ms
    builder.consume(
        sublift::timing::StateEvent{.type = sublift::timing::EventType::Out, .timestamp_ms = 300});

    const auto& segs = builder.segments();
    REQUIRE(segs.size() == 1);
    REQUIRE(segs[0].start_ms == 100);
    REQUIRE(segs[0].end_ms == 300);
    // Anchor should be locked to Frame 2 at 180ms!
    REQUIRE(segs[0].anchor_timestamp_ms == 180);
    REQUIRE_FALSE(segs[0].anchor_image.empty());
    REQUIRE(segs[0].anchor_image.row(0)[0] == 30); // pixel value of frame_2
  }

  SECTION("Anchor fallback priority: delayed > stable > first (short segment)") {
    sublift::timing::TimingConfig cfg{
        .ocr_anchor_delay_ms = 80,
        .ocr_consensus_frames = 4,
        .pure_timing = false,
    };
    sublift::timing::TimelineBuilder builder(cfg);

    auto frame_in = make_test_image(40, 20, sublift::PixelFormat::RGB24, 11);
    auto frame_stable = make_test_image(40, 20, sublift::PixelFormat::RGB24, 22);

    // Segment only has 1 stable frame before OUT: delayed anchor (requires 80ms) not reached!
    builder.consume(
        sublift::timing::StateEvent{.type = sublift::timing::EventType::In, .timestamp_ms = 1000},
        frame_in.view());
    builder.on_stable_frame(frame_stable.view(), 1040);
    builder.consume(
        sublift::timing::StateEvent{.type = sublift::timing::EventType::Out, .timestamp_ms = 1100});

    const auto& segs = builder.segments();
    REQUIRE(segs.size() == 1);
    // Falls back to stable frame!
    REQUIRE(segs[0].anchor_timestamp_ms == 1040);
    REQUIRE(segs[0].anchor_image.row(0)[0] == 22);

    // Reset and test fallback to first frame (0 stable frames before OUT)
    builder.reset();
    builder.consume(
        sublift::timing::StateEvent{.type = sublift::timing::EventType::In, .timestamp_ms = 2000},
        frame_in.view());
    builder.consume(
        sublift::timing::StateEvent{.type = sublift::timing::EventType::Out, .timestamp_ms = 2080});

    const auto& segs2 = builder.segments();
    REQUIRE(segs2.size() == 1);
    REQUIRE(segs2[0].anchor_timestamp_ms == 2000);
    REQUIRE(segs2[0].anchor_image.row(0)[0] == 11);
  }
}

TEST_CASE("TimelineBuilder consensus sampling and middle-slot circular replacement",
          "[timing][timeline][consensus]") {
  sublift::timing::TimingConfig cfg{
      .ocr_anchor_delay_ms = 40,
      .ocr_consensus_frames = 4,
      .pure_timing = false,
  };
  sublift::timing::TimelineBuilder builder(cfg);

  auto dummy = make_test_image(20, 10, sublift::PixelFormat::RGB24, 50);

  builder.consume(
      sublift::timing::StateEvent{.type = sublift::timing::EventType::In, .timestamp_ms = 100},
      dummy.view());

  // Feed 9 consecutive stable frames
  for (int64_t ts = 200; ts <= 1000; ts += 100) {
    builder.on_stable_frame(dummy.view(), ts);
  }

  // Duplicate timestamp feed test: duplicate ts = 1000 should not increase entries
  builder.on_stable_frame(dummy.view(), 1000);

  builder.consume(
      sublift::timing::StateEvent{.type = sublift::timing::EventType::Out, .timestamp_ms = 1100});

  const auto& segs = builder.segments();
  REQUIRE(segs.size() == 1);
  const auto& seg = segs[0];

  // Consensus capacity is 4. Anchor is one timestamp, fallback timestamps must not exceed capacity
  // and must cover wide temporal range (e.g. earliest 100, latest 1000).
  REQUIRE(seg.anchor_timestamp_ms == 200); // delay 1 frame locked at 200ms
  REQUIRE_FALSE(seg.fallback_timestamps_ms.empty());
  REQUIRE(seg.fallback_timestamps_ms.size() <= 4);

  // Check temporal bounds in fallback timestamps
  bool has_early = false;
  bool has_late = false;
  for (int64_t ts : seg.fallback_timestamps_ms) {
    if (ts == 100) has_early = true;
    if (ts == 1000) has_late = true;
  }
  REQUIRE(has_early);
  REQUIRE(has_late);
}

TEST_CASE("TimelineBuilder pure timing mode and finalize open segment at EOF",
          "[timing][timeline][pure_timing]") {
  SECTION("Pure timing mode outputs empty image buffers to conserve memory") {
    sublift::timing::TimingConfig cfg{
        .pure_timing = true,
    };
    sublift::timing::TimelineBuilder builder(cfg);

    auto dummy = make_test_image(40, 20, sublift::PixelFormat::RGB24, 88);

    builder.consume(
        sublift::timing::StateEvent{.type = sublift::timing::EventType::In, .timestamp_ms = 1000},
        dummy.view());
    builder.on_stable_frame(dummy.view(), 1200);
    builder.consume(
        sublift::timing::StateEvent{
            .type = sublift::timing::EventType::Change, .timestamp_ms = 2500, .prev_end_ms = 2500},
        dummy.view());
    builder.consume(
        sublift::timing::StateEvent{.type = sublift::timing::EventType::Out, .timestamp_ms = 4000});

    const auto& segs = builder.segments();
    REQUIRE(segs.size() == 2);
    REQUIRE(segs[0].start_ms == 1000);
    REQUIRE(segs[0].end_ms == 2500);
    REQUIRE(segs[0].anchor_image.empty());
    REQUIRE(segs[0].fallback_images.empty());

    REQUIRE(segs[1].start_ms == 2500);
    REQUIRE(segs[1].end_ms == 4000);
    REQUIRE(segs[1].anchor_image.empty());
  }

  SECTION("finalize closes open segment at EOF") {
    sublift::timing::TimelineBuilder builder;
    builder.consume(
        sublift::timing::StateEvent{.type = sublift::timing::EventType::In, .timestamp_ms = 5000});
    // Video reaches EOF at 7500ms without explicit Out event
    builder.finalize(7500);

    const auto& segs = builder.segments();
    REQUIRE(segs.size() == 1);
    REQUIRE(segs[0].start_ms == 5000);
    REQUIRE(segs[0].end_ms == 7500);
  }

  SECTION("Segment::clone and TimelineBuilder::build semantics") {
    sublift::timing::TimelineBuilder builder;
    builder.consume(
        sublift::timing::StateEvent{.type = sublift::timing::EventType::In, .timestamp_ms = 100});
    builder.consume(
        sublift::timing::StateEvent{.type = sublift::timing::EventType::Out, .timestamp_ms = 300});

    // Test build() const & (creates deep clones)
    auto cloned_segs = builder.build();
    REQUIRE(cloned_segs.size() == 1);
    REQUIRE(builder.segments().size() == 1); // builder still retains its segments

    // Test build() && (moves out)
    auto moved_segs = std::move(builder).build();
    REQUIRE(moved_segs.size() == 1);
  }
}

TEST_CASE("ChangePointDetector respects configured aspect_ratio_threshold", "[timing][aspect]") {
  // Frame size 360x64 => aspect = 5.625 (between 4.0 and 10.0)
  sublift::ImageBuffer frame(360, 64, sublift::PixelFormat::RGB24);
  for (std::int32_t y = 0; y < 64; ++y) {
    auto* row = frame.row(y);
    for (std::int32_t x = 0; x < 360; ++x) {
      row[x * 3 + 0] = 30;
      row[x * 3 + 1] = 30;
      row[x * 3 + 2] = 30;
    }
  }
  // Stamp bright corner noise outside center subtitle corridor (x in [4, 28], y in [4, 20])
  for (std::int32_t y = 4; y < 20; ++y) {
    auto* row = frame.row(y);
    for (std::int32_t x = 4; x < 28; ++x) {
      if ((x % 4) < 2) {
        row[x * 3 + 0] = 245;
        row[x * 3 + 1] = 245;
        row[x * 3 + 2] = 245;
      }
    }
  }

  sublift::timing::TimingConfig wide_cfg;
  wide_cfg.aspect_ratio_threshold = 4.0; // 5.625 > 4.0 => wide adaptive corridor rejects corner noise
  auto wide_analyzed = sublift::timing::analyze_frame(frame.view(), wide_cfg, 0, 100);

  sublift::timing::TimingConfig tall_cfg;
  tall_cfg.aspect_ratio_threshold = 10.0; // 5.625 <= 10.0 => tall fixed mask sees corner strokes
  auto tall_analyzed = sublift::timing::analyze_frame(frame.view(), tall_cfg, 0, 100);

  CHECK(wide_analyzed.foreground_ratio() < tall_analyzed.foreground_ratio());
  CHECK(tall_analyzed.foreground_ratio() > 0.005);
}

TEST_CASE("Timing analyze_frame and process_analyzed_frame equivalence", "[timing]") {
  SECTION("analyze_frame visual facade and AnalyzedFrame lifecycle") {
    sublift::timing::TimingConfig cfg{
        .presence_threshold = 0.01,
        .aspect_ratio_threshold = 6.0,
        .pure_timing = false,
    };

    sublift::ImageView empty_view{};
    auto analyzed_empty = sublift::timing::analyze_frame(empty_view, cfg, 0, 0);
    REQUIRE(analyzed_empty.frame_index == 0);
    REQUIRE(analyzed_empty.timestamp_ms == 0);
    REQUIRE(analyzed_empty.dhash64 == 0ULL);
    REQUIRE(analyzed_empty.presence_ratio == 0.0f);
    REQUIRE_FALSE(analyzed_empty.has_subtitle);
    REQUIRE(analyzed_empty.roi_image.empty());
    REQUIRE(analyzed_empty.roi_mask.empty());

    sublift::ImageBuffer blank(128, 64, sublift::PixelFormat::RGB24);
    for (int y = 0; y < 64; ++y) {
      std::uint8_t* row = blank.row(y);
      for (int x = 0; x < 128; ++x) {
        row[x * 3 + 0] = 40;
        row[x * 3 + 1] = 40;
        row[x * 3 + 2] = 40;
      }
    }
    auto analyzed_blank = sublift::timing::analyze_frame(blank.view(), cfg, 1, 100);
    REQUIRE(analyzed_blank.frame_index == 1);
    REQUIRE(analyzed_blank.timestamp_ms == 100);
    REQUIRE(analyzed_blank.presence_ratio < 0.01f);
    REQUIRE_FALSE(analyzed_blank.has_subtitle);
    REQUIRE_FALSE(analyzed_blank.roi_image.empty());
    REQUIRE_FALSE(analyzed_blank.roi_mask.empty());

    sublift::ImageBuffer subA_img = blank.clone();
    for (int y = 44; y < 60; ++y) {
      std::uint8_t* row = subA_img.row(y);
      for (int x = 48; x < 80; ++x) {
        row[x * 3 + 0] = 240;
        row[x * 3 + 1] = 240;
        row[x * 3 + 2] = 240;
      }
    }
    auto analyzed_sub = sublift::timing::analyze_frame(subA_img.view(), cfg, 2, 200);
    REQUIRE(analyzed_sub.frame_index == 2);
    REQUIRE(analyzed_sub.timestamp_ms == 200);
    REQUIRE(analyzed_sub.presence_ratio >= 0.01f);
    REQUIRE(analyzed_sub.dhash64 != 0ULL);
    REQUIRE(analyzed_sub.has_subtitle);
    REQUIRE_FALSE(analyzed_sub.roi_image.empty());
    REQUIRE_FALSE(analyzed_sub.roi_mask.empty());
    REQUIRE(analyzed_sub.roi_image.width() == 128);
    REQUIRE(analyzed_sub.roi_image.height() == 64);

    sublift::timing::TimingConfig pure_cfg = cfg;
    pure_cfg.pure_timing = true;
    auto analyzed_pure = sublift::timing::analyze_frame(subA_img.view(), pure_cfg, 3, 300);
    REQUIRE(analyzed_pure.has_subtitle);
    REQUIRE(analyzed_pure.roi_image.empty());
    REQUIRE_FALSE(analyzed_pure.roi_mask.empty());

    auto sub_clone = subA_img.clone();
    auto analyzed_moved = sublift::timing::analyze_frame(std::move(sub_clone), cfg, 4, 400);
    REQUIRE(analyzed_moved.has_subtitle);
    REQUIRE_FALSE(analyzed_moved.roi_image.empty());
    REQUIRE(analyzed_moved.roi_image.width() == 128);

    auto cloned = analyzed_sub.clone();
    REQUIRE(cloned.frame_index == analyzed_sub.frame_index);
    REQUIRE(cloned.timestamp_ms == analyzed_sub.timestamp_ms);
    REQUIRE(cloned.dhash64 == analyzed_sub.dhash64);
    REQUIRE(cloned.presence_ratio == analyzed_sub.presence_ratio);
    REQUIRE(cloned.has_subtitle == analyzed_sub.has_subtitle);
    REQUIRE_FALSE(cloned.roi_image.empty());
    REQUIRE_FALSE(cloned.roi_mask.empty());
  }

  SECTION("ChangePointDetector::process_analyzed_frame transitions and parity") {
    sublift::timing::TimingConfig cfg{
        .hysteresis_ms = 200,
        .confirm_ms = 200,
        .change_threshold = 5,
        .presence_threshold = 0.01,
        .enable_ssim_patrol = false,
    };

    sublift::timing::ChangePointDetector det_analyzed(cfg);
    sublift::timing::ChangePointDetector det_direct(cfg);

    sublift::ImageBuffer empty_buf(128, 64, sublift::PixelFormat::RGB24);
    for (int y = 0; y < 64; ++y) {
      std::uint8_t* row = empty_buf.row(y);
      for (int x = 0; x < 128; ++x) {
        row[x * 3 + 0] = 40;
        row[x * 3 + 1] = 40;
        row[x * 3 + 2] = 40;
      }
    }

    sublift::ImageBuffer subA_buf = empty_buf.clone();
    for (int y = 44; y < 60; ++y) {
      std::uint8_t* row = subA_buf.row(y);
      for (int x = 48; x < 80; ++x) {
        row[x * 3 + 0] = 240;
        row[x * 3 + 1] = 240;
        row[x * 3 + 2] = 240;
      }
    }

    sublift::ImageBuffer subB_buf = empty_buf.clone();
    for (int y = 46; y < 58; ++y) {
      std::uint8_t* row = subB_buf.row(y);
      for (int x = 40; x < 88; ++x) {
        row[x * 3 + 0] = 240;
        row[x * 3 + 1] = 240;
        row[x * 3 + 2] = 240;
      }
    }

    struct StreamStep {
      std::int64_t ts;
      const sublift::ImageBuffer& img;
    };

    const std::vector<StreamStep> stream = {
        {0, empty_buf},
        {100, subA_buf},
        {350, subA_buf},
        {500, subB_buf},
        {750, subB_buf},
        {1000, empty_buf},
        {1250, empty_buf},
    };

    std::vector<sublift::timing::StateEvent> events_analyzed;
    std::vector<sublift::timing::StateEvent> events_direct;

    std::uint64_t idx = 0;
    for (const auto& step : stream) {
      auto ev1 = det_direct.process_frame(step.img.view(), step.ts);
      if (ev1.has_value()) {
        events_direct.push_back(*ev1);
      }

      auto analyzed = sublift::timing::analyze_frame(step.img.view(), cfg, idx++, step.ts);
      auto ev2 = det_analyzed.process_analyzed_frame(analyzed);
      if (ev2.has_value()) {
        events_analyzed.push_back(*ev2);
      }
    }

    REQUIRE(events_analyzed.size() == 3);
    REQUIRE(events_direct.size() == 3);
    REQUIRE(events_analyzed == events_direct);

    REQUIRE(events_analyzed[0].type == sublift::timing::EventType::In);
    REQUIRE(events_analyzed[0].timestamp_ms == 100);

    REQUIRE(events_analyzed[1].type == sublift::timing::EventType::Change);
    REQUIRE(events_analyzed[1].timestamp_ms == 500);

    REQUIRE(events_analyzed[2].type == sublift::timing::EventType::Out);
    REQUIRE(events_analyzed[2].timestamp_ms == 1000);
  }

  SECTION("Wide strip pure_timing=true matches pure_timing=false across 1-frame gap and SSIM change") {
    constexpr int w = 640;
    constexpr int h = 64; // aspect = 10.0 > 6.0

    auto make_stroked_cue = [&](int x0, int x1) {
      sublift::ImageBuffer buf(w, h, sublift::PixelFormat::RGB24);
      for (int y = 0; y < h; ++y) {
        std::memset(buf.row(y), 30, static_cast<std::size_t>(w * 3));
      }
      for (int y = 20; y < 44; ++y) {
        std::uint8_t* row = buf.row(y);
        for (int x = x0; x < x1; ++x) {
          if ((x % 5) < 2 || (y < 23 || y >= 41)) {
            row[x * 3 + 0] = 240;
            row[x * 3 + 1] = 240;
            row[x * 3 + 2] = 240;
          }
        }
      }
      return buf;
    };

    sublift::ImageBuffer empty_wide(w, h, sublift::PixelFormat::RGB24);
    for (int y = 0; y < h; ++y) {
      std::memset(empty_wide.row(y), 30, static_cast<std::size_t>(w * 3));
    }
    auto cueA = make_stroked_cue(220, 420);
    auto cueB = make_stroked_cue(180, 320);
    auto cueC = make_stroked_cue(330, 470);

    sublift::timing::TimingConfig full_cfg{
        .hysteresis_ms = 200,
        .confirm_ms = 200,
        .patrol_interval_ms = 400,
        .change_threshold = 10,
        .presence_threshold = 0.01,
        .enable_ssim_patrol = true,
        .ssim_patrol_threshold = 0.92,
        .ssim_patrol_use_mask = true,
        .aspect_ratio_threshold = 6.0,
        .pure_timing = false,
    };
    sublift::timing::TimingConfig pure_cfg = full_cfg;
    pure_cfg.pure_timing = true;

    sublift::timing::ChangePointDetector det_full(full_cfg);
    sublift::timing::ChangePointDetector det_pure(pure_cfg);
    sublift::timing::TimelineBuilder tl_full(full_cfg);
    sublift::timing::TimelineBuilder tl_pure(pure_cfg);

    struct Step {
      std::int64_t ts;
      const sublift::ImageBuffer& img;
    };
    const std::vector<Step> stream = {
        {0, empty_wide},
        {200, cueA},  {400, cueA}, {600, cueA},
        {800, empty_wide}, // 1-frame (200ms) gap at 5fps -> must close cueA
        {1000, cueB}, {1200, cueB}, {1400, cueB},
        {1600, cueC}, {1800, cueC}, // 0-gap direct change
        {2000, empty_wide},
        {2200, empty_wide},
    };

    std::vector<sublift::timing::StateEvent> events_full;
    std::vector<sublift::timing::StateEvent> events_pure;
    std::uint64_t idx = 0;

    for (const auto& step : stream) {
      auto af_full = sublift::timing::analyze_frame(step.img.view(), full_cfg, idx, step.ts);
      auto ev_full = det_full.process_analyzed_frame(af_full);
      if (ev_full.has_value()) {
        events_full.push_back(*ev_full);
        tl_full.consume(*ev_full, af_full.roi_image.view(), step.ts);
      } else {
        tl_full.on_stable_frame(af_full.roi_image.view(), step.ts);
      }

      auto af_pure = sublift::timing::analyze_frame(step.img.view(), pure_cfg, idx, step.ts);
      REQUIRE(af_pure.roi_image.empty());
      REQUIRE_FALSE(af_pure.roi_mask.empty());
      auto ev_pure = det_pure.process_analyzed_frame(af_pure);
      REQUIRE_FALSE(det_pure.anchor_crop().has_value());
      if (ev_pure.has_value()) {
        events_pure.push_back(*ev_pure);
        tl_pure.consume(*ev_pure, af_pure.roi_image.view(), step.ts);
      } else {
        tl_pure.on_stable_frame(af_pure.roi_image.view(), step.ts);
      }
      ++idx;
    }

    tl_full.finalize(2200);
    tl_pure.finalize(2200);

    REQUIRE(events_pure == events_full);
    REQUIRE(tl_full.segments().size() == 3);
    REQUIRE(tl_pure.segments().size() == 3);
    for (std::size_t i = 0; i < 3; ++i) {
      REQUIRE(tl_pure.segments()[i].start_ms == tl_full.segments()[i].start_ms);
      REQUIRE(tl_pure.segments()[i].end_ms == tl_full.segments()[i].end_ms);
      REQUIRE(tl_pure.segments()[i].anchor_image.empty());
    }
    REQUIRE(tl_pure.segments()[0].start_ms == 200);
    REQUIRE(tl_pure.segments()[0].end_ms == 800);
    REQUIRE(tl_pure.segments()[1].start_ms == 1000);
    REQUIRE(tl_pure.segments()[1].end_ms == 1600);
    REQUIRE(tl_pure.segments()[2].start_ms == 1600);
    REQUIRE(tl_pure.segments()[2].end_ms == 2000);

    // Verify same-dHash visual change via patrol works identically without crop image
    det_full.reset();
    det_pure.reset();
    auto maskB = sublift::features::compute_binary_mask(cueB.view(), {.aspect_ratio_threshold = 6.0});
    auto maskC = sublift::features::compute_binary_mask(cueC.view(), {.aspect_ratio_threshold = 6.0});
    constexpr std::uint64_t kSameHash = 0x1234567890ABCDEFULL;

    const std::vector<std::tuple<std::int64_t, const sublift::ImageBuffer&, const sublift::ImageBuffer&>> patrol_stream = {
        {0, cueB, maskB},
        {200, cueB, maskB},
        {600, cueC, maskC},
        {800, cueC, maskC},
        {1000, cueC, maskC},
    };
    std::vector<sublift::timing::StateEvent> p_full, p_pure;
    for (const auto& [ts, crop, mask] : patrol_stream) {
      sublift::features::FrameSignature sig{ts, 0.15, kSameHash};
      if (auto ev = det_full.process(sig, crop.view(), mask.view())) {
        p_full.push_back(*ev);
      }
      if (auto ev = det_pure.process(sig, sublift::ImageView{}, mask.view())) {
        p_pure.push_back(*ev);
      }
    }
    REQUIRE(p_full == p_pure);
    REQUIRE(p_pure.size() == 2);
    REQUIRE(p_pure[0].type == sublift::timing::EventType::In);
    REQUIRE(p_pure[1].type == sublift::timing::EventType::Change);
    REQUIRE(p_pure[1].timestamp_ms == 600);
  }

  SECTION("pure_timing retain_midpoint_in_pure_timing captures thinned midpoint frame without altering timings") {
    constexpr int w = 640;
    constexpr int h = 64;

    auto make_frame = [&](int tag_idx) {
      sublift::ImageBuffer buf(w, h, sublift::PixelFormat::RGB24);
      for (int y = 0; y < h; ++y) {
        std::memset(buf.row(y), 30, static_cast<std::size_t>(w * 3));
      }
      if (tag_idx >= 0) {
        // Subtle low-luminance timestamp tag on pixel (0, 0) invisible to mask/dHash/SSIM
        std::uint8_t* top_row = buf.row(0);
        top_row[0] = static_cast<std::uint8_t>(10 + tag_idx);
        top_row[1] = 5;
        top_row[2] = 5;
        for (int y = 20; y < 44; ++y) {
          std::uint8_t* row = buf.row(y);
          for (int x = 180; x < 460; ++x) {
            if ((x % 5) < 2 || (y < 23 || y >= 41)) {
              row[x * 3 + 0] = 240;
              row[x * 3 + 1] = 240;
              row[x * 3 + 2] = 240;
            }
          }
        }
      }
      return buf;
    };

    sublift::timing::TimingConfig base_cfg{
        .hysteresis_ms = 200,
        .confirm_ms = 200,
        .patrol_interval_ms = 400,
        .change_threshold = 10,
        .presence_threshold = 0.01,
        .enable_ssim_patrol = true,
        .ssim_patrol_threshold = 0.92,
        .ssim_patrol_use_mask = true,
        .aspect_ratio_threshold = 6.0,
        .pure_timing = true,
        .retain_midpoint_in_pure_timing = false,
    };
    sublift::timing::TimingConfig mid_cfg = base_cfg;
    mid_cfg.retain_midpoint_in_pure_timing = true;

    sublift::timing::ChangePointDetector det_base(base_cfg);
    sublift::timing::ChangePointDetector det_mid(mid_cfg);
    sublift::timing::TimelineBuilder tl_base(base_cfg);
    sublift::timing::TimelineBuilder tl_mid(mid_cfg);

    // 40 subtitle frames from 200ms..8000ms (>16 cap to exercise even-index thinning)
    std::uint64_t idx = 0;
    auto push_step = [&](std::int64_t ts, const sublift::ImageBuffer& img) {
      auto af_base = sublift::timing::analyze_frame(img.view(), base_cfg, idx, ts);
      REQUIRE(af_base.roi_image.empty());
      auto ev_base = det_base.process_analyzed_frame(af_base);
      if (ev_base.has_value()) {
        tl_base.consume(*ev_base, af_base.roi_image.view(), ts);
      } else {
        tl_base.on_stable_frame(af_base.roi_image.view(), ts);
      }

      auto af_mid = sublift::timing::analyze_frame(img.view(), mid_cfg, idx, ts);
      REQUIRE_FALSE(af_mid.roi_image.empty());
      auto ev_mid = det_mid.process_analyzed_frame(af_mid);
      REQUIRE_FALSE(det_mid.anchor_crop().has_value());
      if (ev_mid.has_value()) {
        tl_mid.consume(*ev_mid, af_mid.roi_image.view(), ts);
      } else {
        tl_mid.on_stable_frame(af_mid.roi_image.view(), ts);
      }
      ++idx;
    };

    const auto empty = make_frame(-1);
    push_step(0, empty);
    for (int k = 0; k < 40; ++k) {
      push_step(200 + static_cast<std::int64_t>(k) * 200, make_frame(k));
    }
    push_step(8200, empty);
    push_step(8400, empty);

    tl_base.finalize(8400);
    tl_mid.finalize(8400);

    REQUIRE(tl_base.segments().size() == 1);
    REQUIRE(tl_mid.segments().size() == 1);
    REQUIRE(tl_mid.segments()[0].start_ms == tl_base.segments()[0].start_ms);
    REQUIRE(tl_mid.segments()[0].end_ms == tl_base.segments()[0].end_ms);
    REQUIRE(tl_base.segments()[0].anchor_image.empty());

    const auto& anchor = tl_mid.segments()[0].anchor_image;
    REQUIRE_FALSE(anchor.empty());
    REQUIRE(anchor.width() == w);
    REQUIRE(anchor.height() == h);
    REQUIRE(anchor.format() == sublift::PixelFormat::RGB24);

    // Midpoint of [200, 8200] is 4200ms (k = 20); even-index thinning retains around k=20
    const int captured_k = static_cast<int>(anchor.view().row(0)[0]) - 10;
    const std::int64_t captured_ts = 200 + static_cast<std::int64_t>(captured_k) * 200;
    const std::int64_t mid_ms = tl_mid.segments()[0].start_ms +
                                (tl_mid.segments()[0].end_ms - tl_mid.segments()[0].start_ms) / 2;
    REQUIRE(anchor.view().row(0)[1] == 5);
    REQUIRE(captured_k >= 0);
    REQUIRE(captured_k < 40);
    REQUIRE(std::abs(captured_ts - mid_ms) <= 600);
  }
}

