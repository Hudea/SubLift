#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <random>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <sublift/core/color.hpp>
#include <sublift/core/image_buffer.hpp>
#include <sublift/core/image_view.hpp>
#include <sublift/features/dhash.hpp>
#include <sublift/features/signature.hpp>
#include <sublift/timing/changepoint.hpp>
#include <sublift/timing/segment.hpp>
#include <sublift/timing/timeline.hpp>
#include <sublift/timing/timing_config.hpp>

namespace {

sublift::ImageBuffer make_solid_image(int width, int height, sublift::PixelFormat format, uint8_t val) {
  sublift::ImageBuffer buf(width, height, format);
  const int bpp = (format == sublift::PixelFormat::RGB24) ? 3 : 1;
  for (int y = 0; y < height; ++y) {
    std::memset(buf.row(y), val, static_cast<std::size_t>(width * bpp));
  }
  return buf;
}

} // namespace

// ============================================================================
// 1. HIGH-FREQUENCY JITTER & FLICKER STRESS
// ============================================================================

TEST_CASE("Adversarial: High-frequency flicker suppression below hysteresis window",
          "[timing][stress][jitter]") {
  sublift::timing::TimingConfig cfg{
      .hysteresis_ms = 200,
      .confirm_ms = 200,
      .patrol_interval_ms = 600,
      .change_threshold = 10,
      .presence_threshold = 0.05,
      .enable_ssim_patrol = false,
      .enable_ssim_verify = false,
  };
  sublift::timing::ChangePointDetector det(cfg);

  SECTION("Alternating presence/absence every 33ms (30fps frame-by-frame flicker)") {
    // 5000 frames: on, off, on, off...
    // Total duration ~165,000ms (~2.75 minutes)
    std::int64_t t = 0;
    std::size_t event_count = 0;

    for (int i = 0; i < 5000; ++i) {
      bool present = (i % 2 == 0);
      auto ev = det.process(sublift::features::FrameSignature{
          .timestamp_ms = t,
          .foreground_ratio = present ? 0.15 : 0.0,
          .dhash = 0xAAAAAAAAAAAAAAAAULL,
      });
      if (ev.has_value()) {
        ++event_count;
      }
      CHECK(det.current_state() == sublift::timing::State::Empty);
      t += 33;
    }

    REQUIRE(event_count == 0);
    REQUIRE(det.current_state() == sublift::timing::State::Empty);
  }

  SECTION("Pulse train just below hysteresis threshold (190ms ON, 100ms OFF)") {
    // 190ms ON < 200ms hysteresis -> Must NEVER trigger IN
    std::int64_t t = 0;
    std::size_t event_count = 0;

    for (int cycle = 0; cycle < 100; ++cycle) {
      // 190ms ON sampled every 30ms (t, t+30, t+60, t+90, t+120, t+150, t+180)
      for (int step = 0; step < 7; ++step) {
        auto ev = det.process(sublift::features::FrameSignature{
            .timestamp_ms = t + step * 30, // 0, 30, 60, 90, 120, 150, 180 (elapsed = 180 < 200)
            .foreground_ratio = 0.12,
            .dhash = 0x123456789ABCDEF0ULL,
        });
        if (ev.has_value()) ++event_count;
      }
      t += 190;

      // 100ms OFF sampled at t, t+30, t+60
      for (int step = 0; step < 3; ++step) {
        auto ev = det.process(sublift::features::FrameSignature{
            .timestamp_ms = t + step * 30,
            .foreground_ratio = 0.0,
            .dhash = 0ULL,
        });
        if (ev.has_value()) ++event_count;
      }
      t += 100;
      REQUIRE(det.current_state() == sublift::timing::State::Empty);
    }

    REQUIRE(event_count == 0);
  }

  SECTION("Transient absence dropout while in Stable state (50ms gap)") {
    // Enter stable
    det.process(sublift::features::FrameSignature{.timestamp_ms = 0, .foreground_ratio = 0.15, .dhash = 0xFFULL});
    auto in_ev = det.process(sublift::features::FrameSignature{.timestamp_ms = 250, .foreground_ratio = 0.15, .dhash = 0xFFULL});
    REQUIRE(in_ev.has_value());
    REQUIRE(det.current_state() == sublift::timing::State::Stable);

    // Repeated transient dropouts: 50ms absent, then 500ms present
    std::int64_t t = 300;
    std::size_t out_count = 0;
    for (int i = 0; i < 50; ++i) {
      // 50ms dropout: 1 frame at t
      auto ev = det.process(sublift::features::FrameSignature{.timestamp_ms = t, .foreground_ratio = 0.0, .dhash = 0ULL});
      if (ev.has_value()) ++out_count;

      // back to present: t+50ms, t+150ms, t+500ms
      t += 50;
      ev = det.process(sublift::features::FrameSignature{.timestamp_ms = t, .foreground_ratio = 0.15, .dhash = 0xFFULL});
      if (ev.has_value()) ++out_count;

      t += 450;
      ev = det.process(sublift::features::FrameSignature{.timestamp_ms = t, .foreground_ratio = 0.15, .dhash = 0xFFULL});
      if (ev.has_value()) ++out_count;

      REQUIRE(det.current_state() == sublift::timing::State::Stable);
    }
    REQUIRE(out_count == 0);
  }

  SECTION("Rapid hash mutation during onset prevents premature In event") {
    // Subtitle candidate appears, but hash changes completely every 30ms (unstable onset)
    std::mt19937_64 rng(42);
    std::int64_t t = 0;
    std::size_t event_count = 0;

    for (int i = 0; i < 50; ++i) {
      uint64_t random_hash = rng();
      auto ev = det.process(sublift::features::FrameSignature{
          .timestamp_ms = t,
          .foreground_ratio = 0.15,
          .dhash = random_hash,
      });
      if (ev.has_value()) ++event_count;
      t += 30;
    }

    // Since hash was wildly unstable, it should NOT trigger In event despite 1500ms of presence
    REQUIRE(event_count == 0);
    REQUIRE(det.current_state() == sublift::timing::State::Empty);
  }
}

// ============================================================================
// 2. TIMESTAMP EDGE CASES & MONOTONICITY
// ============================================================================

TEST_CASE("Adversarial: Timestamp edge cases (duplicate, large delta, negative, extremes)",
          "[timing][stress][timestamp]") {
  sublift::timing::TimingConfig cfg{
      .hysteresis_ms = 200,
      .confirm_ms = 200,
      .patrol_interval_ms = 600,
      .change_threshold = 10,
      .presence_threshold = 0.05,
      .enable_ssim_patrol = false,
      .enable_ssim_verify = false,
  };
  sublift::timing::ChangePointDetector det(cfg);

  SECTION("Duplicate timestamps do not trigger false state progression in ms_mode") {
    // 100 identical timestamps at t=1000ms
    std::size_t event_count = 0;
    for (int i = 0; i < 100; ++i) {
      auto ev = det.process(sublift::features::FrameSignature{
          .timestamp_ms = 1000,
          .foreground_ratio = 0.20,
          .dhash = 0x1111222233334444ULL,
      });
      if (ev.has_value()) ++event_count;
    }
    // In ms_mode, elapsed = 1000 - 1000 = 0 < 200ms -> Never confirms!
    REQUIRE(event_count == 0);
    REQUIRE(det.current_state() == sublift::timing::State::Empty);

    // Now timestamp advances to 1200ms -> Confirms!
    auto ev = det.process(sublift::features::FrameSignature{
        .timestamp_ms = 1200,
        .foreground_ratio = 0.20,
        .dhash = 0x1111222233334444ULL,
    });
    REQUIRE(ev.has_value());
    REQUIRE(ev->type == sublift::timing::EventType::In);
    REQUIRE(ev->timestamp_ms == 1000); // Backtracked to initial presence
  }

  SECTION("Huge timestamp interval delta_t -> infinity") {
    det.reset();
    // Frame 1 at t=0
    det.process(sublift::features::FrameSignature{
        .timestamp_ms = 0,
        .foreground_ratio = 0.20,
        .dhash = 0x1111ULL,
    });

    // Frame 2 at t = 1,000,000,000,000 ms (~31 years later)
    const std::int64_t huge_ts = 1'000'000'000'000LL;
    auto ev = det.process(sublift::features::FrameSignature{
        .timestamp_ms = huge_ts,
        .foreground_ratio = 0.20,
        .dhash = 0x1111ULL,
    });
    REQUIRE(ev.has_value());
    REQUIRE(ev->type == sublift::timing::EventType::In);
    REQUIRE(ev->timestamp_ms == 0);
    REQUIRE(det.current_state() == sublift::timing::State::Stable);

    // Frame 3 at huge_ts + 250ms -> Out
    det.process(sublift::features::FrameSignature{
        .timestamp_ms = huge_ts + 10,
        .foreground_ratio = 0.0,
        .dhash = 0ULL,
    });
    auto out_ev = det.process(sublift::features::FrameSignature{
        .timestamp_ms = huge_ts + 250,
        .foreground_ratio = 0.0,
        .dhash = 0ULL,
    });
    REQUIRE(out_ev.has_value());
    REQUIRE(out_ev->type == sublift::timing::EventType::Out);
    REQUIRE(out_ev->timestamp_ms == huge_ts + 10);
  }

  SECTION("Negative timestamps (media with negative PTS)") {
    det.reset();
    // Frame at -300ms
    det.process(sublift::features::FrameSignature{
        .timestamp_ms = -300,
        .foreground_ratio = 0.20,
        .dhash = 0x1111ULL,
    });
    // Frame at -100ms: elapsed = -100 - (-300) = 200ms >= 200ms
    auto ev = det.process(sublift::features::FrameSignature{
        .timestamp_ms = -100,
        .foreground_ratio = 0.20,
        .dhash = 0x1111ULL,
    });
    REQUIRE(ev.has_value());
    REQUIRE(ev->type == sublift::timing::EventType::In);
    REQUIRE(ev->timestamp_ms == -300);
    REQUIRE(det.current_state() == sublift::timing::State::Stable);
  }

  SECTION("Out-of-order (backwards) timestamps do not cause crashes or undefined behavior") {
    det.reset();
    // Timestamps monotonically decreasing: 1000, 900, 800, 700...
    for (std::int64_t t = 1000; t >= 0; t -= 50) {
      REQUIRE_NOTHROW(det.process(sublift::features::FrameSignature{
          .timestamp_ms = t,
          .foreground_ratio = 0.20,
          .dhash = 0x1111ULL,
      }));
    }
    // Backward time progression cannot accumulate positive elapsed presence
    REQUIRE(det.current_state() == sublift::timing::State::Empty);
  }
}

// ============================================================================
// 3. BOUNDARY TRANSITIONS & TIMELINE INTEGRATION
// ============================================================================

TEST_CASE("Adversarial: State transitions and TimelineBuilder multi-change integrity",
          "[timing][stress][transitions]") {
  sublift::timing::TimingConfig cfg{
      .hysteresis_ms = 200,
      .confirm_ms = 200,
      .patrol_interval_ms = 600,
      .change_threshold = 10,
      .presence_threshold = 0.05,
      .enable_ssim_patrol = false,
      .enable_ssim_verify = false,
      .ocr_anchor_delay_ms = 400,
      .ocr_consensus_frames = 4,
  };
  sublift::timing::ChangePointDetector det(cfg);
  sublift::timing::TimelineBuilder builder(cfg);

  SECTION("50 consecutive subtitle text changes without silence gaps") {
    // 0ms -> In at 0ms (confirmed at 200ms)
    // Then every 1000ms: dHash changes completely
    std::int64_t t = 0;
    uint64_t current_hash = 0x1111111111111111ULL;

    det.process(sublift::features::FrameSignature{.timestamp_ms = t, .foreground_ratio = 0.15, .dhash = current_hash});
    t += 200;
    auto in_ev = det.process(sublift::features::FrameSignature{.timestamp_ms = t, .foreground_ratio = 0.15, .dhash = current_hash});
    REQUIRE(in_ev.has_value());
    REQUIRE(in_ev->type == sublift::timing::EventType::In);
    builder.consume(*in_ev);

    int num_changes = 50;
    for (int i = 0; i < num_changes; ++i) {
      // Simulate stable display for 800ms
      t += 800;
      builder.on_stable_frame(sublift::ImageView{}, t);

      // Mutate hash
      current_hash ^= (0xDEADBEEF00000000ULL + i * 0x10101010ULL);
      if (current_hash == 0) current_hash = 0xABCDEFULL;

      // Candidate onset
      t += 50;
      auto cand_ev = det.process(sublift::features::FrameSignature{.timestamp_ms = t, .foreground_ratio = 0.15, .dhash = current_hash});
      REQUIRE_FALSE(cand_ev.has_value());

      // Candidate confirmation (after 200ms)
      t += 200;
      auto change_ev = det.process(sublift::features::FrameSignature{.timestamp_ms = t, .foreground_ratio = 0.15, .dhash = current_hash});
      REQUIRE(change_ev.has_value());
      REQUIRE(change_ev->type == sublift::timing::EventType::Change);
      REQUIRE(change_ev->prev_end_ms.has_value());
      REQUIRE(*change_ev->prev_end_ms == change_ev->timestamp_ms);

      builder.consume(*change_ev);
    }

    // Now silence -> Out
    t += 500;
    det.process(sublift::features::FrameSignature{.timestamp_ms = t, .foreground_ratio = 0.0, .dhash = 0ULL});
    t += 200;
    auto out_ev = det.process(sublift::features::FrameSignature{.timestamp_ms = t, .foreground_ratio = 0.0, .dhash = 0ULL});
    REQUIRE(out_ev.has_value());
    REQUIRE(out_ev->type == sublift::timing::EventType::Out);
    builder.consume(*out_ev);

    auto segments = builder.build();
    REQUIRE(segments.size() == static_cast<std::size_t>(num_changes + 1));

    // Verify mathematical invariants of the timeline:
    // 1. Each segment start_ms < end_ms
    // 2. Continuous abutting: segments[i].end_ms == segments[i+1].start_ms
    for (std::size_t i = 0; i < segments.size(); ++i) {
      CHECK(segments[i].start_ms < segments[i].end_ms);
      CHECK(segments[i].duration_ms() > 0);
      if (i > 0) {
        CHECK(segments[i - 1].end_ms == segments[i].start_ms);
      }
    }
  }
}

// ============================================================================
// 4. SSIM PATROL, GLYPH RETENTION VETO & EXTREME DIMENSIONS
// ============================================================================

TEST_CASE("Adversarial: SSIM Patrol & Glyph Retention Veto with edge case geometries and values",
          "[timing][stress][ssim][geometry]") {
  sublift::timing::TimingConfig cfg{
      .hysteresis_ms = 200,
      .confirm_ms = 200,
      .patrol_interval_ms = 600,
      .change_threshold = 10,
      .presence_threshold = 0.05,
      .enable_ssim_patrol = true,
      .ssim_patrol_threshold = 0.92,
      .ssim_patrol_use_mask = true,
      .enable_ssim_verify = true,
      .ssim_threshold = 0.95,
      .ssim_window_size = 7,
      .aspect_ratio_threshold = 6.0,
  };
  sublift::timing::ChangePointDetector det(cfg);

  SECTION("Extreme aspect ratio: very wide (1920x20, aspect=96) with text content") {
    // A wide strip that contains foreground text pixels
    auto crop = make_solid_image(1920, 20, sublift::PixelFormat::RGB24, 10);
    auto mask = make_solid_image(1920, 20, sublift::PixelFormat::Gray8, 0);
    // Draw some text pixels in mask
    for (int y = 5; y < 15; ++y) {
      uint8_t* row = mask.row(y);
      for (int x = 100; x < 500; ++x) {
        row[x] = 255;
      }
    }
    sublift::ImageView crop_view = crop.view();
    sublift::ImageView mask_view = mask.view();

    // In
    det.process(
        sublift::features::FrameSignature{.timestamp_ms = 0, .foreground_ratio = 0.15, .dhash = 0x1ULL},
        crop_view, mask_view);
    auto ev = det.process(
        sublift::features::FrameSignature{.timestamp_ms = 200, .foreground_ratio = 0.15, .dhash = 0x1ULL},
        crop_view, mask_view);
    REQUIRE(ev.has_value());
    REQUIRE(ev->type == sublift::timing::EventType::In);
    REQUIRE(det.current_state() == sublift::timing::State::Stable);
  }

  SECTION("Wide strip with zero mask foreground pixels is held in Empty (documented caveat)") {
    // When a wide strip has 0 mask pixels (e.g. flat background), two-way overlap returns false,
    // holding the detector in Empty to suppress false detections on flat background artifacts.
    det.reset();
    auto crop = make_solid_image(1920, 20, sublift::PixelFormat::RGB24, 200);
    sublift::ImageView view = crop.view();

    det.process(sublift::features::FrameSignature{.timestamp_ms = 0, .foreground_ratio = 0.15, .dhash = 0x1ULL}, view);
    auto ev = det.process(sublift::features::FrameSignature{.timestamp_ms = 200, .foreground_ratio = 0.15, .dhash = 0x1ULL}, view);
    CHECK_FALSE(ev.has_value());
    CHECK(det.current_state() == sublift::timing::State::Empty);
  }

  SECTION("Extreme aspect ratio: very tall (2x1080, aspect=0.00185)") {
    det.reset();
    auto crop = make_solid_image(2, 1080, sublift::PixelFormat::RGB24, 200);
    sublift::ImageView view = crop.view();

    det.process(sublift::features::FrameSignature{.timestamp_ms = 0, .foreground_ratio = 0.15, .dhash = 0x1ULL}, view);
    auto ev = det.process(sublift::features::FrameSignature{.timestamp_ms = 200, .foreground_ratio = 0.15, .dhash = 0x1ULL}, view);
    REQUIRE(ev.has_value());
    REQUIRE(det.current_state() == sublift::timing::State::Stable);
  }

  SECTION("Small image dimension (3x3 and 5x5 smaller than window_size=7) does not crash") {
    det.reset();
    auto crop = make_solid_image(5, 5, sublift::PixelFormat::RGB24, 200);
    sublift::ImageView view = crop.view();

    REQUIRE_NOTHROW(det.process(
        sublift::features::FrameSignature{.timestamp_ms = 0, .foreground_ratio = 0.15, .dhash = 0x1ULL},
        view));
    REQUIRE_NOTHROW(det.process(
        sublift::features::FrameSignature{.timestamp_ms = 200, .foreground_ratio = 0.15, .dhash = 0x1ULL},
        view));
  }

  SECTION("1x1 image and 0x0 empty image safety") {
    det.reset();
    auto crop1 = make_solid_image(1, 1, sublift::PixelFormat::RGB24, 255);
    sublift::ImageView empty_view{};
    sublift::ImageView view1 = crop1.view();

    REQUIRE_NOTHROW(det.process(
        sublift::features::FrameSignature{.timestamp_ms = 0, .foreground_ratio = 0.15, .dhash = 0x1ULL},
        view1));
    REQUIRE_NOTHROW(det.process(
        sublift::features::FrameSignature{.timestamp_ms = 200, .foreground_ratio = 0.15, .dhash = 0x1ULL},
        empty_view));
  }

  SECTION("NaN and Inf in foreground_ratio handled gracefully without exceptions or NaN state") {
    det.reset();
    const double qnan = std::numeric_limits<double>::quiet_NaN();
    const double inf = std::numeric_limits<double>::infinity();

    // NaN should evaluate as not present (has_subtitle = false)
    REQUIRE_NOTHROW(det.process(sublift::features::FrameSignature{
        .timestamp_ms = 0,
        .foreground_ratio = qnan,
        .dhash = 0x1ULL,
    }));
    CHECK(det.current_state() == sublift::timing::State::Empty);

    // Negative foreground ratio
    REQUIRE_NOTHROW(det.process(sublift::features::FrameSignature{
        .timestamp_ms = 100,
        .foreground_ratio = -0.5,
        .dhash = 0x1ULL,
    }));
    CHECK(det.current_state() == sublift::timing::State::Empty);

    // Infinity should be treated as present (>= threshold)
    REQUIRE_NOTHROW(det.process(sublift::features::FrameSignature{
        .timestamp_ms = 200,
        .foreground_ratio = inf,
        .dhash = 0x1ULL,
    }));
  }

  SECTION("Mismatched valid_mask dimensions fallback safely") {
    det.reset();
    auto crop = make_solid_image(100, 20, sublift::PixelFormat::RGB24, 128);
    // Mask with wrong dimensions (80x15)
    auto bad_mask = make_solid_image(80, 15, sublift::PixelFormat::Gray8, 255);

    // Passing mismatched valid_mask should safely ignore bad_mask and not crash
    REQUIRE_NOTHROW(det.process(
        sublift::features::FrameSignature{.timestamp_ms = 0, .foreground_ratio = 0.15, .dhash = 0x1ULL},
        crop.view(), bad_mask.view()));
    REQUIRE_NOTHROW(det.process(
        sublift::features::FrameSignature{.timestamp_ms = 200, .foreground_ratio = 0.15, .dhash = 0x1ULL},
        crop.view(), bad_mask.view()));
  }

  SECTION("RGB24 valid_mask passed does not cause crash or buffer overrun") {
    det.reset();
    auto crop = make_solid_image(120, 20, sublift::PixelFormat::RGB24, 128);
    auto rgb_mask = make_solid_image(120, 20, sublift::PixelFormat::RGB24, 255);

    REQUIRE_NOTHROW(det.process(
        sublift::features::FrameSignature{.timestamp_ms = 0, .foreground_ratio = 0.15, .dhash = 0x1ULL},
        crop.view(), rgb_mask.view()));
    REQUIRE_NOTHROW(det.process(
        sublift::features::FrameSignature{.timestamp_ms = 200, .foreground_ratio = 0.15, .dhash = 0x1ULL},
        crop.view(), rgb_mask.view()));
  }
}

// ============================================================================
// 5. EVAL CACHE STRESS & RESET REPRODUCIBILITY
// ============================================================================

TEST_CASE("Adversarial: FrameEvalCache capacity overflow and reset reproducibility",
          "[timing][stress][eval_cache]") {
  sublift::timing::TimingConfig cfg{
      .hysteresis_ms = 200,
      .confirm_ms = 200,
      .patrol_interval_ms = 600,
      .change_threshold = 10,
      .presence_threshold = 0.05,
      .enable_ssim_patrol = true,
      .ssim_patrol_threshold = 0.92,
      .ssim_patrol_use_mask = true,
  };
  sublift::timing::ChangePointDetector det(cfg);

  SECTION("FrameEvalCache handles > 4 pairs without buffer overflow") {
    // kMaxEntries is 4. Test querying 10 different pairs in one frame.
    std::vector<sublift::ImageBuffer> masks;
    for (int i = 0; i < 10; ++i) {
      masks.push_back(make_solid_image(64, 16, sublift::PixelFormat::Gray8, static_cast<uint8_t>(20 * i)));
    }

    for (int i = 0; i < 9; ++i) {
      double ssim = det.eval_mask_ssim(masks[i].view(), masks[i + 1].view());
      CHECK(ssim >= 0.0);
      CHECK(ssim <= 1.0);
    }

    // Number of evaluations should be recorded, cache should not overflow
    CHECK(det.eval_cache().count <= sublift::timing::FrameEvalCache::kMaxEntries);
  }

  SECTION("Reset leaves detector in 100% identical state for subsequent runs") {
    auto run_sequence = [&](sublift::timing::ChangePointDetector& d) {
      std::vector<sublift::timing::StateEvent> evs;
      auto ev1 = d.process(sublift::features::FrameSignature{.timestamp_ms = 0, .foreground_ratio = 0.10, .dhash = 0x100ULL});
      if (ev1) evs.push_back(*ev1);
      auto ev2 = d.process(sublift::features::FrameSignature{.timestamp_ms = 250, .foreground_ratio = 0.10, .dhash = 0x100ULL});
      if (ev2) evs.push_back(*ev2);
      auto ev3 = d.process(sublift::features::FrameSignature{.timestamp_ms = 1000, .foreground_ratio = 0.10, .dhash = 0x999ULL});
      if (ev3) evs.push_back(*ev3);
      auto ev4 = d.process(sublift::features::FrameSignature{.timestamp_ms = 1250, .foreground_ratio = 0.10, .dhash = 0x999ULL});
      if (ev4) evs.push_back(*ev4);
      auto ev5 = d.process(sublift::features::FrameSignature{.timestamp_ms = 1500, .foreground_ratio = 0.0, .dhash = 0ULL});
      if (ev5) evs.push_back(*ev5);
      auto ev6 = d.process(sublift::features::FrameSignature{.timestamp_ms = 1750, .foreground_ratio = 0.0, .dhash = 0ULL});
      if (ev6) evs.push_back(*ev6);
      return evs;
    };

    auto run1 = run_sequence(det);
    det.reset();
    auto run2 = run_sequence(det);

    REQUIRE(run1.size() == run2.size());
    for (std::size_t i = 0; i < run1.size(); ++i) {
      CHECK(run1[i] == run2[i]);
    }
  }
}

// ============================================================================
// 6. GLYPH RETENTION VETO EXACT BOUNDARIES & CORNER CASES
// ============================================================================

TEST_CASE("Adversarial: Sector-Aware Glyph Retention Veto exact boundary conditions",
          "[timing][stress][glyph_veto]") {
  sublift::timing::TimingConfig cfg{
      .hysteresis_ms = 200,
      .confirm_ms = 200,
      .patrol_interval_ms = 600,
      .change_threshold = 10,
      .presence_threshold = 0.05,
      .enable_ssim_patrol = false,
      .enable_ssim_verify = false,
      .aspect_ratio_threshold = 6.0,
  };
  sublift::timing::ChangePointDetector det(cfg);

  const int W = 200;
  const int H = 20; // aspect = 10.0 > 6.0

  auto create_mask_with_pixels = [&](int l_pixels, int c_pixels, int r_pixels) {
    auto mask = make_solid_image(W, H, sublift::PixelFormat::Gray8, 0);
    // Left: x in [5, 45) -> 40 columns
    int count = 0;
    for (int y = 0; y < H && count < l_pixels; ++y) {
      uint8_t* row = mask.row(y);
      for (int x = 5; x < 45 && count < l_pixels; ++x) {
        row[x] = 255;
        ++count;
      }
    }
    // Center: x in [60, 140) -> 80 columns
    count = 0;
    for (int y = 0; y < H && count < c_pixels; ++y) {
      uint8_t* row = mask.row(y);
      for (int x = 60; x < 140 && count < c_pixels; ++x) {
        row[x] = 255;
        ++count;
      }
    }
    // Right: x in [155, 195) -> 40 columns
    count = 0;
    for (int y = 0; y < H && count < r_pixels; ++y) {
      uint8_t* row = mask.row(y);
      for (int x = 155; x < 195 && count < r_pixels; ++x) {
        row[x] = 255;
        ++count;
      }
    }
    return mask;
  };

  auto anchor_crop = make_solid_image(W, H, sublift::PixelFormat::RGB24, 50);
  auto anchor_mask = create_mask_with_pixels(100, 100, 100); // 300 pixels total

  // Initial IN
  det.process(sublift::features::FrameSignature{.timestamp_ms = 0, .foreground_ratio = 0.15, .dhash = 0x0000FFFFULL},
              anchor_crop.view(), anchor_mask.view());
  auto in_ev = det.process(sublift::features::FrameSignature{.timestamp_ms = 200, .foreground_ratio = 0.15, .dhash = 0x0000FFFFULL},
                           anchor_crop.view(), anchor_mask.view());
  REQUIRE(in_ev.has_value());
  REQUIRE(det.current_state() == sublift::timing::State::Stable);

  SECTION("Identical mask vetoes dHash change (e.g. dynamic noisy background)") {
    // Current frame has large dHash difference (distance = 32 > 10)
    uint64_t noisy_bg_dhash = 0xFFFF0000ULL;
    auto cur_crop = make_solid_image(W, H, sublift::PixelFormat::RGB24, 180);
    auto cur_mask = create_mask_with_pixels(100, 100, 100); // identical text mask

    // Candidate should NOT be created or should be immediately vetoed
    auto ev1 = det.process(
        sublift::features::FrameSignature{.timestamp_ms = 400, .foreground_ratio = 0.15, .dhash = noisy_bg_dhash},
        cur_crop.view(), cur_mask.view());
    CHECK_FALSE(ev1.has_value());
    CHECK_FALSE(det.change_candidate().has_value()); // Glyph retention veto resets candidate!

    auto ev2 = det.process(
        sublift::features::FrameSignature{.timestamp_ms = 700, .foreground_ratio = 0.15, .dhash = noisy_bg_dhash},
        cur_crop.view(), cur_mask.view());
    CHECK_FALSE(ev2.has_value());
    CHECK(det.current_state() == sublift::timing::State::Stable);
  }

  SECTION("Changed left sector (overlap < 0.65) allows Change event") {
    // Left sector text changes (only 40% overlap in left sector)
    uint64_t diff_hash = 0xFFFF0000ULL;
    auto cur_crop = make_solid_image(W, H, sublift::PixelFormat::RGB24, 180);

    // Left has different pixels: draw from x=25 to 45 instead of 5 to 45
    auto cur_mask = make_solid_image(W, H, sublift::PixelFormat::Gray8, 0);
    // Draw 100 pixels in left at offset positions (low overlap)
    int count = 0;
    for (int y = 0; y < H && count < 100; ++y) {
      uint8_t* row = cur_mask.row(y);
      for (int x = 35; x < 45 && count < 100; ++x) { // only 10 cols overlap out of 40 -> 25% overlap
        row[x] = 255;
        ++count;
      }
    }
    // Center & right identical
    count = 0;
    for (int y = 0; y < H && count < 100; ++y) {
      uint8_t* row = cur_mask.row(y);
      for (int x = 60; x < 140 && count < 100; ++x) {
        row[x] = 255;
        ++count;
      }
    }
    count = 0;
    for (int y = 0; y < H && count < 100; ++y) {
      uint8_t* row = cur_mask.row(y);
      for (int x = 155; x < 195 && count < 100; ++x) {
        row[x] = 255;
        ++count;
      }
    }

    auto ev1 = det.process(
        sublift::features::FrameSignature{.timestamp_ms = 400, .foreground_ratio = 0.15, .dhash = diff_hash},
        cur_crop.view(), cur_mask.view());
    CHECK_FALSE(ev1.has_value());
    CHECK(det.change_candidate().has_value()); // Sector changed -> Candidate WAS created!

    // After confirm_ms (200ms)
    auto ev2 = det.process(
        sublift::features::FrameSignature{.timestamp_ms = 650, .foreground_ratio = 0.15, .dhash = diff_hash},
        cur_crop.view(), cur_mask.view());
    REQUIRE(ev2.has_value());
    REQUIRE(ev2->type == sublift::timing::EventType::Change);
    REQUIRE(det.current_state() == sublift::timing::State::StablePrime);
  }
}

// ============================================================================
// 7. VARIABLE SAMPLING RATES & PURE MILLISECOND INVARIANCE
// ============================================================================

TEST_CASE("Adversarial: ChangePointDetector millisecond invariance across variable frame rates",
          "[timing][stress][variable_fps]") {
  sublift::timing::TimingConfig cfg{
      .hysteresis_ms = 200,
      .confirm_ms = 200,
      .patrol_interval_ms = 600,
      .change_threshold = 10,
      .presence_threshold = 0.05,
      .enable_ssim_patrol = false,
      .enable_ssim_verify = false,
  };

  // Timeline scenario (in ms):
  // [0, 1000): Empty silence
  // [1000, 3000): Subtitle 1 (foreground_ratio = 0.15, dhash = 0xAAAAAAAAAAAAAAAAULL)
  // [3000, 5000): Subtitle 2 (foreground_ratio = 0.15, dhash = 0x5555555555555555ULL)
  // [5000, 6500): Empty silence

  auto get_frame_at = [](std::int64_t t) -> sublift::features::FrameSignature {
    if (t < 1000) {
      return sublift::features::FrameSignature{t, 0.0, 0ULL};
    } else if (t < 3000) {
      return sublift::features::FrameSignature{t, 0.15, 0xAAAAAAAAAAAAAAAAULL};
    } else if (t < 5000) {
      return sublift::features::FrameSignature{t, 0.15, 0x5555555555555555ULL};
    } else {
      return sublift::features::FrameSignature{t, 0.0, 0ULL};
    }
  };

  auto run_detector = [&](const std::vector<std::int64_t>& timestamps) {
    sublift::timing::ChangePointDetector det(cfg);
    std::vector<sublift::timing::StateEvent> events;
    for (auto ts : timestamps) {
      auto ev = det.process(get_frame_at(ts));
      if (ev.has_value()) {
        events.push_back(*ev);
      }
    }
    return events;
  };

  SECTION("5 FPS (200ms step) vs 10 FPS (100ms step) exact millisecond equivalence") {
    std::vector<std::int64_t> ts_5fps;
    for (std::int64_t t = 0; t <= 6400; t += 200) ts_5fps.push_back(t);

    std::vector<std::int64_t> ts_10fps;
    for (std::int64_t t = 0; t <= 6400; t += 100) ts_10fps.push_back(t);

    auto ev_5 = run_detector(ts_5fps);
    auto ev_10 = run_detector(ts_10fps);

    REQUIRE(ev_5.size() == 3);
    REQUIRE(ev_10.size() == 3);

    // Both must detect exact same semantic events
    CHECK(ev_5[0].type == sublift::timing::EventType::In);
    CHECK(ev_5[0].timestamp_ms == 1000);
    CHECK(ev_10[0].type == sublift::timing::EventType::In);
    CHECK(ev_10[0].timestamp_ms == 1000);

    CHECK(ev_5[1].type == sublift::timing::EventType::Change);
    CHECK(ev_5[1].timestamp_ms == 3000);
    CHECK(ev_10[1].type == sublift::timing::EventType::Change);
    CHECK(ev_10[1].timestamp_ms == 3000);

    CHECK(ev_5[2].type == sublift::timing::EventType::Out);
    CHECK(ev_5[2].timestamp_ms == 5000);
    CHECK(ev_10[2].type == sublift::timing::EventType::Out);
    CHECK(ev_10[2].timestamp_ms == 5000);
  }

  SECTION("24 FPS (~41.67ms step) event timestamps match ground truth within 1 sample interval") {
    std::vector<std::int64_t> ts_24fps;
    for (int i = 0; i < 155; ++i) {
      ts_24fps.push_back(static_cast<std::int64_t>(std::round(i * 1000.0 / 24.0)));
    }

    auto ev_24 = run_detector(ts_24fps);
    REQUIRE(ev_24.size() == 3);

    CHECK(ev_24[0].type == sublift::timing::EventType::In);
    CHECK(std::abs(ev_24[0].timestamp_ms - 1000) <= 42);

    CHECK(ev_24[1].type == sublift::timing::EventType::Change);
    CHECK(std::abs(ev_24[1].timestamp_ms - 3000) <= 42);

    CHECK(ev_24[2].type == sublift::timing::EventType::Out);
    CHECK(std::abs(ev_24[2].timestamp_ms - 5000) <= 42);
  }

  SECTION("Irregular jittered timestamps (10ms to 120ms random delta)") {
    std::mt19937 rng(1337);
    std::uniform_int_distribution<std::int64_t> dist(10, 120);

    std::vector<std::int64_t> ts_jitter;
    std::int64_t cur_t = 0;
    while (cur_t <= 6500) {
      ts_jitter.push_back(cur_t);
      cur_t += dist(rng);
    }

    auto ev_jitter = run_detector(ts_jitter);
    REQUIRE(ev_jitter.size() == 3);

    CHECK(ev_jitter[0].type == sublift::timing::EventType::In);
    CHECK(ev_jitter[0].timestamp_ms >= 1000);
    CHECK(ev_jitter[0].timestamp_ms < 1150);

    CHECK(ev_jitter[1].type == sublift::timing::EventType::Change);
    CHECK(ev_jitter[1].timestamp_ms >= 3000);
    CHECK(ev_jitter[1].timestamp_ms < 3150);

    CHECK(ev_jitter[2].type == sublift::timing::EventType::Out);
    CHECK(ev_jitter[2].timestamp_ms >= 5000);
    CHECK(ev_jitter[2].timestamp_ms < 5150);
  }
}

// ============================================================================
// 8. SSIM PATROL RATE INVARIANCE (patrol_interval_ms vs frame count)
// ============================================================================

TEST_CASE("Adversarial: SSIM patrol execution frequency strictly governed by patrol_interval_ms",
          "[timing][stress][patrol_interval]") {
  sublift::timing::TimingConfig cfg{
      .hysteresis_ms = 0,
      .confirm_ms = 200,
      .patrol_interval_ms = 600,
      .change_threshold = 10,
      .presence_threshold = 0.05,
      .enable_ssim_patrol = true,
      .ssim_patrol_threshold = 0.92,
      .ssim_patrol_use_mask = false,
      .enable_ssim_verify = false,
  };

  auto crop = make_solid_image(128, 32, sublift::PixelFormat::RGB24, 100);
  sublift::ImageView crop_view = crop.view();

  SECTION("SSIM computation count must be exactly 0 when enable_ssim_patrol is false") {
    sublift::timing::TimingConfig no_patrol_cfg = cfg;
    no_patrol_cfg.enable_ssim_patrol = false;
    sublift::timing::ChangePointDetector det(no_patrol_cfg);
    det.process(sublift::features::FrameSignature{.timestamp_ms = 0, .foreground_ratio = 0.20, .dhash = 0x1ULL}, crop_view);
    for (std::int64_t t = 100; t <= 2000; t += 100) {
      det.process(sublift::features::FrameSignature{.timestamp_ms = t, .foreground_ratio = 0.20, .dhash = 0x1ULL}, crop_view);
    }
    CHECK(det.eval_cache().ssim_eval_count == 0);
  }

  SECTION("Patrol count across frame rates") {
    auto count_patrols = [&](std::int64_t step_ms) {
      sublift::timing::ChangePointDetector det(cfg);
      det.process(sublift::features::FrameSignature{.timestamp_ms = 0, .foreground_ratio = 0.20, .dhash = 0x1ULL}, crop_view);

      std::size_t patrol_count = 0;
      for (std::int64_t t = step_ms; t <= 6000; t += step_ms) {
        std::size_t ssim_before = det.eval_cache().ssim_eval_count;
        det.process(sublift::features::FrameSignature{.timestamp_ms = t, .foreground_ratio = 0.20, .dhash = 0x1ULL}, crop_view);
        if (det.eval_cache().ssim_eval_count > ssim_before) {
          ++patrol_count;
        }
      }
      return patrol_count;
    };

    std::size_t patrols_5fps = count_patrols(200);   // 31 frames total
    std::size_t patrols_25fps = count_patrols(40);   // 151 frames total
    std::size_t patrols_100fps = count_patrols(10);  // 601 frames total

    CHECK(patrols_5fps == 10);
    CHECK(patrols_25fps == 10);
    CHECK(patrols_100fps == 10);
    REQUIRE(patrols_100fps == patrols_5fps);
  }
}

// ============================================================================
// 9. WIDE ASPECT-RATIO CROP (aspect > 6.0) SSIM PATROL & NORMALIZATION
// ============================================================================

TEST_CASE("Adversarial: Wide aspect-ratio crops (aspect > 6.0) execute SSIM patrol and detect visual changes",
          "[timing][stress][wide_patrol]") {
  sublift::timing::TimingConfig cfg{
      .hysteresis_ms = 0,
      .confirm_ms = 200,
      .patrol_interval_ms = 600,
      .change_threshold = 10,
      .presence_threshold = 0.05,
      .enable_ssim_patrol = true,
      .ssim_patrol_threshold = 0.92,
      .ssim_patrol_use_mask = false,
      .enable_ssim_verify = false,
      .aspect_ratio_threshold = 6.0,
  };
  sublift::timing::ChangePointDetector det(cfg);

  constexpr int W = 640;
  constexpr int H = 32;

  SECTION("Wide frame with dhash=0 is rejected under wide strip protection") {
    auto wide_crop = make_solid_image(W, H, sublift::PixelFormat::RGB24, 128);
    sublift::ImageView view = wide_crop.view();

    auto ev = det.process(
        sublift::features::FrameSignature{.timestamp_ms = 100, .foreground_ratio = 0.25, .dhash = 0ULL},
        view);
    REQUIRE_FALSE(ev.has_value());
    REQUIRE(det.current_state() == sublift::timing::State::Empty);
  }

  SECTION("Wide frame executes SSIM patrol and detects change when dHash distance is below threshold") {
    auto anchor = make_solid_image(W, H, sublift::PixelFormat::RGB24, 0);
    for (int y = 8; y < 24; ++y) {
      uint8_t* row = anchor.row(y);
      for (int x = 100; x < 540; ++x) {
        row[x * 3 + 0] = 255;
        row[x * 3 + 1] = 255;
        row[x * 3 + 2] = 255;
      }
    }

    auto modified = make_solid_image(W, H, sublift::PixelFormat::RGB24, 0);
    for (int y = 8; y < 24; ++y) {
      uint8_t* row = modified.row(y);
      for (int x = 100; x < 540; ++x) {
        if ((x / 4) % 2 == 0) {
          row[x * 3 + 0] = 255;
          row[x * 3 + 1] = 255;
          row[x * 3 + 2] = 255;
        }
      }
    }

    det.process(
        sublift::features::FrameSignature{.timestamp_ms = 0, .foreground_ratio = 0.25, .dhash = 0xAAAAULL},
        anchor.view());
    det.process(
        sublift::features::FrameSignature{.timestamp_ms = 200, .foreground_ratio = 0.25, .dhash = 0xAAAAULL},
        anchor.view());
    REQUIRE(det.current_state() == sublift::timing::State::Stable);

    det.process(
        sublift::features::FrameSignature{.timestamp_ms = 200, .foreground_ratio = 0.25, .dhash = 0xAAAAULL},
        anchor.view());
    det.process(
        sublift::features::FrameSignature{.timestamp_ms = 400, .foreground_ratio = 0.25, .dhash = 0xAAAAULL},
        anchor.view());
    CHECK_FALSE(det.change_candidate().has_value());

    std::size_t ssim_count_before = det.eval_cache().ssim_eval_count;
    det.process(
        sublift::features::FrameSignature{.timestamp_ms = 600, .foreground_ratio = 0.25, .dhash = 0xAAABULL},
        modified.view());

    CHECK(det.eval_cache().ssim_eval_count > ssim_count_before);
    REQUIRE(det.change_candidate().has_value());
    CHECK(det.change_candidate()->timestamp_ms == 600);

    auto ev_change = det.process(
        sublift::features::FrameSignature{.timestamp_ms = 800, .foreground_ratio = 0.25, .dhash = 0xAAABULL},
        modified.view());
    REQUIRE(ev_change.has_value());
    REQUIRE(ev_change->type == sublift::timing::EventType::Change);
    REQUIRE(ev_change->timestamp_ms == 600);
  }
}

// ============================================================================
// 10. DELAYED OCR ANCHOR MS INVARIANCE ACROSS SAMPLING RATES
// ============================================================================

TEST_CASE("Adversarial: TimelineBuilder delayed OCR anchor locked by ocr_anchor_delay_ms across frame rates",
          "[timing][stress][timeline_anchor]") {
  sublift::timing::TimingConfig cfg{
      .hysteresis_ms = 0,
      .confirm_ms = 200,
      .patrol_interval_ms = 600,
      .ocr_anchor_delay_ms = 400,
      .pure_timing = false,
  };

  auto test_anchor_ts = [&](std::int64_t step_ms) -> std::optional<std::int64_t> {
    sublift::timing::TimelineBuilder builder(cfg);
    auto frame = make_solid_image(64, 16, sublift::PixelFormat::RGB24, 255);
    sublift::ImageView view = frame.view();

    builder.consume(sublift::timing::StateEvent{sublift::timing::EventType::In, 100, std::nullopt}, view, 100);

    for (std::int64_t t = 100 + step_ms; t <= 1000; t += step_ms) {
      builder.on_stable_frame(view, t);
    }
    builder.consume(sublift::timing::StateEvent{sublift::timing::EventType::Out, 1000, std::nullopt}, view, 1000);
    auto segs = builder.build();
    if (segs.empty()) return std::nullopt;
    return segs[0].anchor_timestamp_ms;
  };

  auto a_5fps = test_anchor_ts(200);
  REQUIRE(a_5fps.has_value());
  CHECK(*a_5fps == 500);

  auto a_10fps = test_anchor_ts(100);
  REQUIRE(a_10fps.has_value());
  CHECK(*a_10fps == 500);

  auto a_25fps = test_anchor_ts(40);
  REQUIRE(a_25fps.has_value());
  CHECK(*a_25fps == 500);

  auto a_50fps = test_anchor_ts(20);
  REQUIRE(a_50fps.has_value());
  CHECK(*a_50fps == 500);
}


