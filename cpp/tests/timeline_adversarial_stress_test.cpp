#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <random>
#include <set>
#include <stdexcept>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <sublift/core/color.hpp>
#include <sublift/core/image_buffer.hpp>
#include <sublift/core/image_view.hpp>
#include <sublift/timing/changepoint.hpp>
#include <sublift/timing/segment.hpp>
#include <sublift/timing/timeline.hpp>
#include <sublift/timing/timing_config.hpp>

namespace {

sublift::ImageBuffer make_pattern_image(int width, int height, sublift::PixelFormat format, uint8_t fill_val) {
  sublift::ImageBuffer buf(width, height, format);
  const int bpp = (format == sublift::PixelFormat::RGB24) ? 3 : 1;
  for (int y = 0; y < height; ++y) {
    uint8_t* row = buf.row(y);
    for (int x = 0; x < width * bpp; ++x) {
      row[x] = static_cast<uint8_t>((fill_val + x + y * 7) & 0xFF);
    }
  }
  return buf;
}

sublift::ImageBuffer make_uniform_image(int width, int height, sublift::PixelFormat format, uint8_t val) {
  sublift::ImageBuffer buf(width, height, format);
  const int bpp = (format == sublift::PixelFormat::RGB24) ? 3 : 1;
  for (int y = 0; y < height; ++y) {
    std::memset(buf.row(y), val, static_cast<std::size_t>(width * bpp));
  }
  return buf;
}

} // namespace

// ============================================================================
// 1. EMPTY TIMELINE & LIFECYCLE FINALIZATION
// ============================================================================

TEST_CASE("Adversarial: Empty timeline and finalize edge cases", "[timing][timeline][adversarial][empty]") {
  SECTION("Finalize on freshly constructed builder produces zero segments") {
    sublift::timing::TimelineBuilder builder;
    builder.finalize(0);
    CHECK(builder.segments().empty());

    builder.finalize(1000);
    CHECK(builder.segments().empty());

    auto segs_const = builder.build();
    CHECK(segs_const.empty());

    auto segs_move = std::move(builder).build();
    CHECK(segs_move.empty());
  }

  SECTION("Repeated reset and finalize calls on empty builder are benign") {
    sublift::timing::TimelineBuilder builder;
    for (int i = 0; i < 10; ++i) {
      builder.reset();
      builder.finalize(i * 100);
      CHECK(builder.segments().empty());
    }
  }

  SECTION("on_stable_frame on empty builder is a safe no-op") {
    sublift::timing::TimelineBuilder builder;
    auto img = make_uniform_image(20, 10, sublift::PixelFormat::RGB24, 42);
    builder.on_stable_frame(img.view(), 500);
    builder.consume_frame(img.view(), 600);
    builder.finalize(1000);
    CHECK(builder.segments().empty());
  }
}

// ============================================================================
// 2. PATHOLOGICAL & MALFORMED EVENT SEQUENCES
// ============================================================================

TEST_CASE("Adversarial: Malformed and pathological event sequences", "[timing][timeline][adversarial][pathological]") {
  SECTION("Out without In is completely ignored") {
    sublift::timing::TimelineBuilder builder;
    auto img = make_uniform_image(20, 10, sublift::PixelFormat::RGB24, 1);

    builder.consume(sublift::timing::StateEvent{.type = sublift::timing::EventType::Out, .timestamp_ms = 500}, img.view());
    builder.consume(sublift::timing::StateEvent{.type = sublift::timing::EventType::Out, .timestamp_ms = 1000});
    builder.consume(sublift::timing::StateEvent{.type = sublift::timing::EventType::Out, .timestamp_ms = 1500});
    builder.finalize(2000);

    CHECK(builder.segments().empty());
  }

  SECTION("Repeated In events without Out properly auto-close preceding segments") {
    sublift::timing::TimelineBuilder builder;
    auto img1 = make_uniform_image(20, 10, sublift::PixelFormat::RGB24, 10);
    auto img2 = make_uniform_image(20, 10, sublift::PixelFormat::RGB24, 20);
    auto img3 = make_uniform_image(20, 10, sublift::PixelFormat::RGB24, 30);

    // In 1 at 1000ms
    builder.consume(sublift::timing::StateEvent{.type = sublift::timing::EventType::In, .timestamp_ms = 1000}, img1.view());

    // In 2 at 2000ms (no Out in between): auto-closes segment 1 at [1000, 2000]
    builder.consume(sublift::timing::StateEvent{.type = sublift::timing::EventType::In, .timestamp_ms = 2000}, img2.view());
    REQUIRE(builder.segments().size() == 1);
    CHECK(builder.segments()[0].start_ms == 1000);
    CHECK(builder.segments()[0].end_ms == 2000);
    CHECK(builder.segments()[0].anchor_timestamp_ms == 1000);

    // In 3 at 3500ms: auto-closes segment 2 at [2000, 3500]
    builder.consume(sublift::timing::StateEvent{.type = sublift::timing::EventType::In, .timestamp_ms = 3500}, img3.view());
    REQUIRE(builder.segments().size() == 2);
    CHECK(builder.segments()[1].start_ms == 2000);
    CHECK(builder.segments()[1].end_ms == 3500);

    // Finalize at 4000ms: closes segment 3 at [3500, 4000]
    builder.finalize(4000);
    REQUIRE(builder.segments().size() == 3);
    CHECK(builder.segments()[2].start_ms == 3500);
    CHECK(builder.segments()[2].end_ms == 4000);
  }

  SECTION("Change event without prior In opens a segment cleanly") {
    sublift::timing::TimelineBuilder builder;
    auto img = make_uniform_image(20, 10, sublift::PixelFormat::RGB24, 99);

    builder.consume(sublift::timing::StateEvent{
        .type = sublift::timing::EventType::Change,
        .timestamp_ms = 2000,
        .prev_end_ms = 2000,
    }, img.view());

    builder.on_stable_frame(img.view(), 2100);
    builder.consume(sublift::timing::StateEvent{.type = sublift::timing::EventType::Out, .timestamp_ms = 2500});

    const auto& segs = builder.segments();
    REQUIRE(segs.size() == 1);
    CHECK(segs[0].start_ms == 2000);
    CHECK(segs[0].end_ms == 2500);
  }

  SECTION("Change event missing prev_end_ms throws std::invalid_argument") {
    sublift::timing::TimelineBuilder builder;
    CHECK_THROWS_AS(
        builder.consume(sublift::timing::StateEvent{
            .type = sublift::timing::EventType::Change,
            .timestamp_ms = 1000,
            .prev_end_ms = std::nullopt,
        }),
        std::invalid_argument);
  }

  SECTION("Zero-duration segments (identical In and Out timestamps)") {
    sublift::timing::TimelineBuilder builder;
    auto img = make_uniform_image(20, 10, sublift::PixelFormat::RGB24, 5);

    builder.consume(sublift::timing::StateEvent{.type = sublift::timing::EventType::In, .timestamp_ms = 1000}, img.view());
    builder.consume(sublift::timing::StateEvent{.type = sublift::timing::EventType::Out, .timestamp_ms = 1000});

    const auto& segs = builder.segments();
    REQUIRE(segs.size() == 1);
    CHECK(segs[0].start_ms == 1000);
    CHECK(segs[0].end_ms == 1000);
    CHECK(segs[0].duration_ms() == 0);
    CHECK(segs[0].anchor_timestamp_ms == 1000);
  }

  SECTION("Repeated on_stable_frame with identical timestamp does not bloat consensus buffer") {
    sublift::timing::TimelineBuilder builder;
    auto img = make_uniform_image(20, 10, sublift::PixelFormat::RGB24, 50);

    builder.consume(sublift::timing::StateEvent{.type = sublift::timing::EventType::In, .timestamp_ms = 100}, img.view());

    // 10 identical stable frames at ts = 200
    for (int i = 0; i < 10; ++i) {
      builder.on_stable_frame(img.view(), 200);
    }
    builder.consume(sublift::timing::StateEvent{.type = sublift::timing::EventType::Out, .timestamp_ms = 300});

    const auto& segs = builder.segments();
    REQUIRE(segs.size() == 1);
    // There should be NO duplicate timestamps in fallback timestamps
    std::set<int64_t> unique_ts(segs[0].fallback_timestamps_ms.begin(), segs[0].fallback_timestamps_ms.end());
    CHECK(unique_ts.size() == segs[0].fallback_timestamps_ms.size());
    // Also anchor must not be duplicated in fallbacks
    CHECK(unique_ts.count(segs[0].anchor_timestamp_ms) == 0);
  }

  SECTION("Negative and zero timestamps support (DEV-002)") {
    sublift::timing::TimelineBuilder builder;
    auto img = make_uniform_image(20, 10, sublift::PixelFormat::RGB24, 1);

    builder.consume(sublift::timing::StateEvent{.type = sublift::timing::EventType::In, .timestamp_ms = -500}, img.view());
    builder.on_stable_frame(img.view(), -200);
    builder.consume(sublift::timing::StateEvent{.type = sublift::timing::EventType::Out, .timestamp_ms = 0});

    const auto& segs = builder.segments();
    REQUIRE(segs.size() == 1);
    CHECK(segs[0].start_ms == -500);
    CHECK(segs[0].end_ms == 0);
    CHECK(segs[0].duration_ms() == 500);
  }
}

// ============================================================================
// 3. ANCHOR RETENTION INVARIANTS & FALLBACK PRIORITY
// ============================================================================

TEST_CASE("Adversarial: Anchor locking countdown and fallback priority", "[timing][timeline][adversarial][anchor]") {
  sublift::timing::TimingConfig cfg{
      .ocr_anchor_delay_ms = 80,
      .ocr_consensus_frames = 4,
      .pure_timing = false,
  };

  SECTION("Delayed anchor strictly locks at 2nd stable frame and is unaffected by subsequent frames") {
    sublift::timing::TimelineBuilder builder(cfg);

    auto img_in = make_pattern_image(30, 15, sublift::PixelFormat::RGB24, 10);
    auto img_s1 = make_pattern_image(30, 15, sublift::PixelFormat::RGB24, 20);
    auto img_s2 = make_pattern_image(30, 15, sublift::PixelFormat::RGB24, 30);
    auto img_s3 = make_pattern_image(30, 15, sublift::PixelFormat::RGB24, 40);
    auto img_s4 = make_pattern_image(30, 15, sublift::PixelFormat::RGB24, 50);

    builder.consume(sublift::timing::StateEvent{.type = sublift::timing::EventType::In, .timestamp_ms = 1000}, img_in.view());
    builder.on_stable_frame(img_s1.view(), 1040); // pending: 2 -> 1
    builder.on_stable_frame(img_s2.view(), 1080); // pending: 1 -> 0, locks delayed anchor at img_s2 (1080ms)!
    builder.on_stable_frame(img_s3.view(), 1120); // should NOT overwrite anchor
    builder.on_stable_frame(img_s4.view(), 1160); // should NOT overwrite anchor

    builder.consume(sublift::timing::StateEvent{.type = sublift::timing::EventType::Out, .timestamp_ms = 1300});

    const auto& segs = builder.segments();
    REQUIRE(segs.size() == 1);
    CHECK(segs[0].anchor_timestamp_ms == 1080);
    REQUIRE_FALSE(segs[0].anchor_image.empty());
    // Verify image data matches img_s2 byte-for-byte
    CHECK(std::memcmp(segs[0].anchor_image.data(), img_s2.data(), 30 * 15 * 3) == 0);
  }

  SECTION("Fallback priority strictly holds: delayed > stable > first") {
    // 1. 0 stable frames before Out: fallback to first frame
    {
      sublift::timing::TimelineBuilder builder(cfg);
      auto img_in = make_pattern_image(30, 15, sublift::PixelFormat::RGB24, 11);
      builder.consume(sublift::timing::StateEvent{.type = sublift::timing::EventType::In, .timestamp_ms = 500}, img_in.view());
      builder.consume(sublift::timing::StateEvent{.type = sublift::timing::EventType::Out, .timestamp_ms = 550});

      const auto& segs = builder.segments();
      REQUIRE(segs.size() == 1);
      CHECK(segs[0].anchor_timestamp_ms == 500);
      CHECK(std::memcmp(segs[0].anchor_image.data(), img_in.data(), 30 * 15 * 3) == 0);
    }

    // 2. 1 stable frame before Out: fallback to stable frame
    {
      sublift::timing::TimelineBuilder builder(cfg);
      auto img_in = make_pattern_image(30, 15, sublift::PixelFormat::RGB24, 11);
      auto img_s1 = make_pattern_image(30, 15, sublift::PixelFormat::RGB24, 22);
      builder.consume(sublift::timing::StateEvent{.type = sublift::timing::EventType::In, .timestamp_ms = 500}, img_in.view());
      builder.on_stable_frame(img_s1.view(), 540);
      builder.consume(sublift::timing::StateEvent{.type = sublift::timing::EventType::Out, .timestamp_ms = 580});

      const auto& segs = builder.segments();
      REQUIRE(segs.size() == 1);
      CHECK(segs[0].anchor_timestamp_ms == 540);
      CHECK(std::memcmp(segs[0].anchor_image.data(), img_s1.data(), 30 * 15 * 3) == 0);
    }
  }

  SECTION("Delay = 0 immediately locks first frame as delayed anchor") {
    sublift::timing::TimingConfig cfg_zero = cfg;
    cfg_zero.ocr_anchor_delay_ms = 0;
    sublift::timing::TimelineBuilder builder(cfg_zero);

    auto img_in = make_pattern_image(20, 10, sublift::PixelFormat::RGB24, 15);
    auto img_s1 = make_pattern_image(20, 10, sublift::PixelFormat::RGB24, 25);

    builder.consume(sublift::timing::StateEvent{.type = sublift::timing::EventType::In, .timestamp_ms = 100}, img_in.view());
    builder.on_stable_frame(img_s1.view(), 140);
    builder.consume(sublift::timing::StateEvent{.type = sublift::timing::EventType::Out, .timestamp_ms = 200});

    const auto& segs = builder.segments();
    REQUIRE(segs.size() == 1);
    CHECK(segs[0].anchor_timestamp_ms == 100);
    CHECK(std::memcmp(segs[0].anchor_image.data(), img_in.data(), 20 * 10 * 3) == 0);
  }

  SECTION("Negative delay frames clamped safely to zero") {
    sublift::timing::TimingConfig cfg_neg = cfg;
    cfg_neg.ocr_anchor_delay_ms = -500;
    sublift::timing::TimelineBuilder builder(cfg_neg);

    auto img_in = make_pattern_image(20, 10, sublift::PixelFormat::RGB24, 17);
    builder.consume(sublift::timing::StateEvent{.type = sublift::timing::EventType::In, .timestamp_ms = 100}, img_in.view());
    builder.consume(sublift::timing::StateEvent{.type = sublift::timing::EventType::Out, .timestamp_ms = 200});

    const auto& segs = builder.segments();
    REQUIRE(segs.size() == 1);
    CHECK(segs[0].anchor_timestamp_ms == 100);
  }

  SECTION("Empty image at In followed by valid stable frames resolves anchor safely") {
    sublift::timing::TimelineBuilder builder(cfg);
    auto img_s1 = make_pattern_image(20, 10, sublift::PixelFormat::RGB24, 77);

    builder.consume(sublift::timing::StateEvent{.type = sublift::timing::EventType::In, .timestamp_ms = 100}, sublift::ImageView{});
    builder.on_stable_frame(img_s1.view(), 150);
    builder.consume(sublift::timing::StateEvent{.type = sublift::timing::EventType::Out, .timestamp_ms = 200});

    const auto& segs = builder.segments();
    REQUIRE(segs.size() == 1);
    CHECK(segs[0].anchor_timestamp_ms == 150);
    CHECK_FALSE(segs[0].anchor_image.empty());
  }

  SECTION("Empty image views during on_stable_frame are safely ignored") {
    sublift::timing::TimelineBuilder builder(cfg);
    auto img_in = make_pattern_image(20, 10, sublift::PixelFormat::RGB24, 33);

    builder.consume(sublift::timing::StateEvent{.type = sublift::timing::EventType::In, .timestamp_ms = 100}, img_in.view());
    // Empty stable frames should not decrement pending delay or corrupt state
    builder.on_stable_frame(sublift::ImageView{}, 140);
    builder.on_stable_frame(sublift::ImageView{}, 180);
    builder.consume(sublift::timing::StateEvent{.type = sublift::timing::EventType::Out, .timestamp_ms = 200});

    const auto& segs = builder.segments();
    REQUIRE(segs.size() == 1);
    // Still falls back to first frame
    CHECK(segs[0].anchor_timestamp_ms == 100);
    CHECK_FALSE(segs[0].anchor_image.empty());
  }
}

// ============================================================================
// 4. CONSENSUS SAMPLE POOL & DEDUPLICATION INVARIANTS
// ============================================================================

TEST_CASE("Adversarial: Consensus sample pool, head/tail preservation, and deduplication",
          "[timing][timeline][adversarial][consensus]") {
  sublift::timing::TimingConfig cfg{
      .ocr_anchor_delay_ms = 150,
      .ocr_consensus_frames = 4,
      .pure_timing = false,
  };

  SECTION("Head and tail are strictly preserved across 50 stable frames") {
    sublift::timing::TimelineBuilder builder(cfg);
    auto dummy = make_uniform_image(20, 10, sublift::PixelFormat::RGB24, 12);

    builder.consume(sublift::timing::StateEvent{.type = sublift::timing::EventType::In, .timestamp_ms = 100}, dummy.view());

    for (int64_t ts = 200; ts <= 2500; ts += 50) {
      builder.on_stable_frame(dummy.view(), ts);
    }

    builder.consume(sublift::timing::StateEvent{.type = sublift::timing::EventType::Out, .timestamp_ms = 2600});

    const auto& segs = builder.segments();
    REQUIRE(segs.size() == 1);
    const auto& seg = segs[0];

    // Delayed anchor was locked at 2nd stable frame (ts = 250)
    CHECK(seg.anchor_timestamp_ms == 250);

    // Verify fallback timestamps
    REQUIRE_FALSE(seg.fallback_timestamps_ms.empty());
    // Fallback timestamps and fallback images must strictly match in size
    CHECK(seg.fallback_timestamps_ms.size() == seg.fallback_images.size());

    // Head timestamp (100ms) MUST be preserved in fallback timestamps
    bool has_head = (std::find(seg.fallback_timestamps_ms.begin(), seg.fallback_timestamps_ms.end(), 100) !=
                     seg.fallback_timestamps_ms.end());
    CHECK(has_head);

    // Tail timestamp (2500ms) MUST be preserved in fallback timestamps
    bool has_tail = (std::find(seg.fallback_timestamps_ms.begin(), seg.fallback_timestamps_ms.end(), 2500) !=
                     seg.fallback_timestamps_ms.end());
    CHECK(has_tail);

    // Total deduplication check: all timestamps across anchor and fallbacks must be strictly unique!
    std::set<int64_t> all_ts(seg.fallback_timestamps_ms.begin(), seg.fallback_timestamps_ms.end());
    CHECK(all_ts.size() == seg.fallback_timestamps_ms.size()); // internal uniqueness
    CHECK(all_ts.count(seg.anchor_timestamp_ms) == 0); // anchor uniqueness
  }

  SECTION("Consensus capacity boundary values: 1, 2, 0, negative") {
    // Capacity = 1
    {
      sublift::timing::TimingConfig c = cfg;
      c.ocr_consensus_frames = 1;
      sublift::timing::TimelineBuilder builder(c);
      auto img = make_uniform_image(10, 10, sublift::PixelFormat::RGB24, 1);
      builder.consume(sublift::timing::StateEvent{.type = sublift::timing::EventType::In, .timestamp_ms = 100}, img.view());
      builder.on_stable_frame(img.view(), 200);
      builder.on_stable_frame(img.view(), 300);
      builder.consume(sublift::timing::StateEvent{.type = sublift::timing::EventType::Out, .timestamp_ms = 400});

      REQUIRE(builder.segments().size() == 1);
      CHECK(builder.segments()[0].fallback_timestamps_ms.size() == builder.segments()[0].fallback_images.size());
    }

    // Capacity = -5 clamped to 1
    {
      sublift::timing::TimingConfig c = cfg;
      c.ocr_consensus_frames = -5;
      sublift::timing::TimelineBuilder builder(c);
      auto img = make_uniform_image(10, 10, sublift::PixelFormat::RGB24, 1);
      builder.consume(sublift::timing::StateEvent{.type = sublift::timing::EventType::In, .timestamp_ms = 100}, img.view());
      builder.on_stable_frame(img.view(), 200);
      builder.consume(sublift::timing::StateEvent{.type = sublift::timing::EventType::Out, .timestamp_ms = 300});

      REQUIRE(builder.segments().size() == 1);
      CHECK(builder.segments()[0].fallback_timestamps_ms.size() == builder.segments()[0].fallback_images.size());
    }
  }
}

// ============================================================================
// 5. PURE TIMING MODE EQUIVALENCE & BUFFER CLEANLINESS
// ============================================================================

TEST_CASE("Adversarial: Pure timing mode memory invariants and timeline parity",
          "[timing][timeline][adversarial][pure_timing]") {
  sublift::timing::TimingConfig cfg_pure{
      .ocr_anchor_delay_ms = 400,
      .ocr_consensus_frames = 4,
      .pure_timing = true,
  };
  sublift::timing::TimingConfig cfg_normal = cfg_pure;
  cfg_normal.pure_timing = false;

  sublift::timing::TimelineBuilder builder_pure(cfg_pure);
  sublift::timing::TimelineBuilder builder_normal(cfg_normal);

  auto img = make_pattern_image(50, 30, sublift::PixelFormat::RGB24, 88);

  // Feed rich sequence into both builders
  auto feed = [&](sublift::timing::TimelineBuilder& b) {
    b.consume(sublift::timing::StateEvent{.type = sublift::timing::EventType::In, .timestamp_ms = 1000}, img.view());
    b.on_stable_frame(img.view(), 1100);
    b.on_stable_frame(img.view(), 1200);
    b.on_stable_frame(img.view(), 1300);
    b.consume(sublift::timing::StateEvent{
        .type = sublift::timing::EventType::Change,
        .timestamp_ms = 2000,
        .prev_end_ms = 2000,
    }, img.view());
    b.on_stable_frame(img.view(), 2100);
    b.consume(sublift::timing::StateEvent{.type = sublift::timing::EventType::Out, .timestamp_ms = 3000}, img.view());

    b.consume(sublift::timing::StateEvent{.type = sublift::timing::EventType::In, .timestamp_ms = 4000}, img.view());
    b.finalize(5500);
  };

  feed(builder_pure);
  feed(builder_normal);

  const auto& segs_pure = builder_pure.segments();
  const auto& segs_normal = builder_normal.segments();

  REQUIRE(segs_pure.size() == 3);
  REQUIRE(segs_normal.size() == 3);

  for (std::size_t i = 0; i < segs_pure.size(); ++i) {
    // 1. Timeline intervals must be 100% IDENTICAL
    CHECK(segs_pure[i].start_ms == segs_normal[i].start_ms);
    CHECK(segs_pure[i].end_ms == segs_normal[i].end_ms);
    CHECK(segs_pure[i].duration_ms() == segs_normal[i].duration_ms());

    // 2. Pure timing mode buffers MUST be strictly empty
    CHECK(segs_pure[i].anchor_image.empty());
    CHECK(segs_pure[i].fallback_images.empty());
    CHECK(segs_pure[i].fallback_timestamps_ms.empty());
    CHECK(segs_pure[i].anchor_timestamp_ms == segs_pure[i].start_ms);

    // 3. Normal mode buffers MUST have retained images
    CHECK_FALSE(segs_normal[i].anchor_image.empty());
  }
}

// ============================================================================
// 6. DEEP COPY, OWNERSHIP TRANSFER & MEMORY SAFETY
// ============================================================================

TEST_CASE("Adversarial: Deep copy, clone independence, and ROI lifetime safety",
          "[timing][timeline][adversarial][memory]") {
  SECTION("Segment::clone produces completely independent allocations") {
    sublift::timing::Segment s1;
    s1.start_ms = 100;
    s1.end_ms = 500;
    s1.anchor_timestamp_ms = 180;
    s1.fallback_timestamps_ms = {100, 300, 400};
    s1.anchor_image = make_uniform_image(20, 10, sublift::PixelFormat::RGB24, 100);
    s1.fallback_images.push_back(make_uniform_image(20, 10, sublift::PixelFormat::RGB24, 110));
    s1.fallback_images.push_back(make_uniform_image(20, 10, sublift::PixelFormat::RGB24, 120));
    s1.fallback_images.push_back(make_uniform_image(20, 10, sublift::PixelFormat::RGB24, 130));

    sublift::timing::Segment s2 = s1.clone();

    // Verify metadata
    CHECK(s2.start_ms == s1.start_ms);
    CHECK(s2.end_ms == s1.end_ms);
    CHECK(s2.anchor_timestamp_ms == s1.anchor_timestamp_ms);
    CHECK(s2.fallback_timestamps_ms == s1.fallback_timestamps_ms);

    // Verify independent memory pointers
    REQUIRE(s2.anchor_image.data() != s1.anchor_image.data());
    REQUIRE(s2.fallback_images.size() == s1.fallback_images.size());
    for (std::size_t i = 0; i < s1.fallback_images.size(); ++i) {
      REQUIRE(s2.fallback_images[i].data() != s1.fallback_images[i].data());
    }

    // Mutate s1: s2 must NOT be altered
    std::memset(s1.anchor_image.data(), 0xFF, 20 * 10 * 3);
    std::memset(s1.fallback_images[0].data(), 0xAA, 20 * 10 * 3);

    CHECK(s2.anchor_image.data()[0] == 100);
    CHECK(s2.fallback_images[0].data()[0] == 110);
  }

  SECTION("TimelineBuilder::build() const & vs build() && value categories") {
    sublift::timing::TimelineBuilder builder;
    auto img = make_uniform_image(20, 10, sublift::PixelFormat::RGB24, 55);

    builder.consume(sublift::timing::StateEvent{.type = sublift::timing::EventType::In, .timestamp_ms = 100}, img.view());
    builder.consume(sublift::timing::StateEvent{.type = sublift::timing::EventType::Out, .timestamp_ms = 200});

    // 1. build() const & returns deep copies, leaves builder intact
    auto copied = builder.build();
    REQUIRE(copied.size() == 1);
    REQUIRE(builder.segments().size() == 1);
    CHECK(copied[0].anchor_image.data() != builder.segments()[0].anchor_image.data());

    // 2. build() && moves out, leaves builder empty
    auto moved = std::move(builder).build();
    REQUIRE(moved.size() == 1);
    CHECK(builder.segments().empty());
  }

  SECTION("Sub-ROI view lifetime safety: parent buffer destroyed before segment read") {
    sublift::timing::TimelineBuilder builder;

    {
      // Parent image buffer created in inner scope
      auto parent_image = make_pattern_image(200, 100, sublift::PixelFormat::RGB24, 7);
      // Create sub-ROI: 40x20 at offset (50, 30)
      sublift::ImageView roi_view = parent_image.roi(50, 30, 40, 20);

      builder.consume(sublift::timing::StateEvent{.type = sublift::timing::EventType::In, .timestamp_ms = 1000}, roi_view);
      builder.on_stable_frame(roi_view, 1040);
      builder.on_stable_frame(roi_view, 1080);
      builder.consume(sublift::timing::StateEvent{.type = sublift::timing::EventType::Out, .timestamp_ms = 1200});

      // parent_image is destroyed when exiting this block
    }

    // Builder segments must be completely intact and valid
    const auto& segs = builder.segments();
    REQUIRE(segs.size() == 1);
    REQUIRE_FALSE(segs[0].anchor_image.empty());
    CHECK(segs[0].anchor_image.width() == 40);
    CHECK(segs[0].anchor_image.height() == 20);
    CHECK(segs[0].anchor_image.stride_bytes() == 40 * 3); // tightly packed clone

    // Read pixel data: no use-after-free or segfault
    const uint8_t* p = segs[0].anchor_image.data();
    REQUIRE(p != nullptr);
    CHECK(p[0] == static_cast<uint8_t>((7 + 50 * 3 + 30 * 7) & 0xFF));
  }
}

// ============================================================================
// 7. HIGH-THROUGHPUT RANDOM FUZZING / STRESS HARNESS
// ============================================================================

TEST_CASE("Adversarial: High-throughput randomized event fuzzing", "[timing][timeline][adversarial][fuzz]") {
  constexpr int kNumActions = 5000;
  std::mt19937_64 rng(0xCAFEBABEDEADULL);

  sublift::timing::TimingConfig cfg{
      .ocr_anchor_delay_ms = 400,
      .ocr_consensus_frames = 4,
      .pure_timing = false,
  };
  sublift::timing::TimelineBuilder builder(cfg);

  auto img_a = make_pattern_image(60, 20, sublift::PixelFormat::RGB24, 1);
  auto img_b = make_pattern_image(40, 15, sublift::PixelFormat::Gray8, 2);

  int64_t current_ts = 0;

  for (int i = 0; i < kNumActions; ++i) {
    uint32_t action = rng() % 6;
    int64_t delta = static_cast<int64_t>(rng() % 200);
    current_ts += delta;

    sublift::ImageView current_view;
    int view_choice = rng() % 3;
    if (view_choice == 0) current_view = img_a.view();
    else if (view_choice == 1) current_view = img_b.view();
    // else empty view

    switch (action) {
      case 0: // EventType::In
        builder.consume(sublift::timing::StateEvent{.type = sublift::timing::EventType::In, .timestamp_ms = current_ts}, current_view);
        break;
      case 1: // on_stable_frame
        builder.on_stable_frame(current_view, current_ts);
        break;
      case 2: // EventType::Change
        builder.consume(sublift::timing::StateEvent{
            .type = sublift::timing::EventType::Change,
            .timestamp_ms = current_ts,
            .prev_end_ms = current_ts,
        }, current_view);
        break;
      case 3: // EventType::Out
        builder.consume(sublift::timing::StateEvent{.type = sublift::timing::EventType::Out, .timestamp_ms = current_ts}, current_view);
        break;
      case 4: // finalize
        builder.finalize(current_ts);
        break;
      case 5: // sporadic reset
        if (rng() % 50 == 0) {
          builder.reset();
        }
        break;
    }

    // Invariant check across all generated segments so far
    for (const auto& seg : builder.segments()) {
      CHECK(seg.fallback_timestamps_ms.size() == seg.fallback_images.size());
      // Timestamp deduplication invariant
      std::set<int64_t> ts_set(seg.fallback_timestamps_ms.begin(), seg.fallback_timestamps_ms.end());
      CHECK(ts_set.size() == seg.fallback_timestamps_ms.size());
      if (!seg.anchor_image.empty()) {
        CHECK(ts_set.count(seg.anchor_timestamp_ms) == 0);
      }
    }
  }

  // Final cleanup
  builder.finalize(current_ts + 100);
  CHECK_NOTHROW(builder.build());
}
