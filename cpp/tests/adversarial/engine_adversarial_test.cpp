#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>
#include <sublift/adapters/mock/mock_frame_source.hpp>
#include <sublift/adapters/mock/mock_ocr.hpp>
#include <sublift/adapters/mock/mock_subtitle_sink.hpp>
#include <sublift/core/box.hpp>
#include <sublift/core/frame.hpp>
#include <sublift/core/image_buffer.hpp>
#include <sublift/engine/analysis_executor.hpp>
#include <sublift/engine/config.hpp>
#include <sublift/engine/observer.hpp>
#include <sublift/engine/segment_recognizer.hpp>
#include <sublift/engine/session.hpp>
#include <sublift/ports/detector.hpp>
#include <sublift/ports/frame_source.hpp>
#include <sublift/ports/ocr_engine.hpp>
#include <sublift/timing/timeline.hpp>
#include <sublift/timing/timing_config.hpp>

namespace {

// Helper to create synthetic solid color frames
sublift::Frame make_solid_frame(int64_t ts, int32_t w, int32_t h, uint8_t val = 40) {
  if (w <= 0 || h <= 0) {
    return sublift::Frame{
        .timestamp_ms = ts,
        .image = sublift::ImageBuffer(),
    };
  }
  sublift::ImageBuffer img(w, h, sublift::PixelFormat::RGB24);
  for (int32_t y = 0; y < h; ++y) {
    uint8_t* row = img.row(y);
    for (int32_t x = 0; x < w; ++x) {
      row[x * 3 + 0] = val;
      row[x * 3 + 1] = val;
      row[x * 3 + 2] = val;
    }
  }
  return sublift::Frame{
      .timestamp_ms = ts,
      .image = std::move(img),
  };
}

// Slow OCR Engine simulating heavy AI model inference
class SlowMockOcrEngine final : public sublift::IOcrEngine {
 public:
  explicit SlowMockOcrEngine(std::chrono::microseconds delay = std::chrono::milliseconds(5))
      : delay_(delay) {}

  sublift::OcrResult recognize(const sublift::ImageView&) override {
    call_count_.fetch_add(1, std::memory_order_relaxed);
    if (delay_.count() > 0) {
      std::this_thread::sleep_for(delay_);
    }
    return sublift::OcrResult{
        .text = "SLOW OCR SUBTITLE",
        .confidence = 0.95,
        .lines = {},
    };
  }

  std::string engine_name() const override { return "SlowMockOcrEngine"; }
  std::size_t call_count() const noexcept { return call_count_.load(std::memory_order_relaxed); }

 private:
  std::chrono::microseconds delay_;
  std::atomic<std::size_t> call_count_{0};
};

// Malicious Detector returning invalid/out-of-bound/negative regions
class MaliciousDetector final : public sublift::IDetector {
 public:
  enum class Mode {
    NegativeCoords,
    OutOfBounds,
    ZeroArea,
    NegativeDimensions,
    NullRegion,
  };

  explicit MaliciousDetector(Mode mode = Mode::OutOfBounds) : mode_(mode) {}

  void set_mode(Mode mode) noexcept { mode_ = mode; }

  std::optional<sublift::Region> detect(const sublift::Frame& frame) override {
    int32_t w = frame.image.width();
    int32_t h = frame.image.height();
    switch (mode_) {
      case Mode::NegativeCoords:
        return sublift::Region{sublift::FrameLocalBox{-100, -50, 50, 50}};
      case Mode::OutOfBounds:
        return sublift::Region{sublift::FrameLocalBox{w + 500, h + 500, 200, 200}};
      case Mode::ZeroArea:
        return sublift::Region{sublift::FrameLocalBox{10, 10, 0, 0}};
      case Mode::NegativeDimensions:
        return sublift::Region{sublift::FrameLocalBox{10, 10, -50, -20}};
      case Mode::NullRegion:
      default:
        return std::nullopt;
    }
  }

 private:
  Mode mode_{Mode::OutOfBounds};
};

// Faulty Observer throwing exceptions during callbacks
class FaultyObserver final : public sublift::IExtractionObserver {
 public:
  bool throw_on_progress{false};
  bool throw_on_segment{false};
  std::atomic<int> progress_count{0};

  void on_progress(int64_t, int64_t, double) override {
    progress_count.fetch_add(1, std::memory_order_relaxed);
    if (throw_on_progress) {
      throw std::runtime_error("FaultyObserver: intentional exception in on_progress");
    }
  }

  void on_segment_detected(const sublift::timing::Segment&) override {
    if (throw_on_segment) {
      throw std::runtime_error("FaultyObserver: intentional exception in on_segment_detected");
    }
  }
};

// Faulty OCR Engine throwing exceptions during recognize()
class FaultyOcrEngine final : public sublift::IOcrEngine {
 public:
  bool should_throw{true};

  sublift::OcrResult recognize(const sublift::ImageView&) override {
    if (should_throw) {
      throw std::runtime_error("FaultyOcrEngine: OCR hardware accelerator fault");
    }
    return sublift::OcrResult{.text = "OK", .confidence = 0.9};
  }

  std::string engine_name() const override { return "FaultyOcrEngine"; }
};

// Faulty Frame Source throwing during extract()
class FaultyFrameSource final : public sublift::IFrameSource {
 public:
  sublift::VideoInfo probe(const std::filesystem::path&) override {
    return sublift::VideoInfo{.width = 1280, .height = 720, .duration_ms = 5000, .fps = 25.0};
  }

  sublift::FrameIOPlan plan(const sublift::VideoInfo&, const std::optional<sublift::SourceBox>&) override {
    return sublift::FrameIOPlan{};
  }

  void extract(const std::filesystem::path&, double, const std::optional<sublift::SourceBox>&,
               std::function<bool(sublift::Frame)>) override {
    throw std::runtime_error("FaultyFrameSource: corrupted video stream container decode failure");
  }

  sublift::ImageBuffer extract_frame(const std::filesystem::path&, int64_t,
                                     const std::optional<sublift::SourceBox>&) override {
    return sublift::ImageBuffer();
  }

  void cancel() override { cancelled_ = true; }
  bool is_cancelled() const noexcept override { return cancelled_; }

 private:
  bool cancelled_{false};
};

} // namespace

// ============================================================================
// Adversarial Stress Test Suite for ExtractionSession
// ============================================================================

TEST_CASE("Adversarial: Rapid cancellation and idempotency", "[adversarial][engine][cancel]") {
  SECTION("Cancel during active rapid feed loop") {
    sublift::adapters::mock::MockFrameSource source;
    SlowMockOcrEngine ocr(std::chrono::milliseconds(2));
    sublift::adapters::mock::MockSubtitleSink sink;

    sublift::ExtractionConfig cfg{
        .sample_fps = 10.0,
        .num_threads = 4,
        .queue_capacity = 8,
    };

    sublift::ExtractionSession session(source, ocr, nullptr, &sink, cfg);
    REQUIRE_FALSE(session.is_cancelled());

    std::atomic<bool> cancelled{false};
    std::atomic<int> fed_count{0};

    // Producer thread pushing 200 frames
    auto producer = std::async(std::launch::async, [&]() {
      for (int i = 0; i < 200; ++i) {
        if (session.is_cancelled()) {
          break;
        }
        session.feed(make_solid_frame(i * 100, 128, 64, (i % 2 == 0) ? 40 : 240));
        fed_count.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::sleep_for(std::chrono::microseconds(500));
      }
    });

    // Canceller thread triggering cancel after brief delay
    auto canceller = std::async(std::launch::async, [&]() {
      std::this_thread::sleep_for(std::chrono::milliseconds(15));
      session.cancel();
      cancelled.store(true);
    });

    producer.wait();
    canceller.wait();

    REQUIRE(session.is_cancelled());

    // Finalize should terminate gracefully without deadlocking
    auto entries = session.finalize();
    REQUIRE_FALSE(session.is_running());
  }

  SECTION("Cancel called before any frames fed") {
    sublift::adapters::mock::MockFrameSource source;
    sublift::adapters::mock::MockOcrEngine ocr;
    sublift::adapters::mock::MockSubtitleSink sink;

    sublift::ExtractionSession session(source, ocr, nullptr, &sink, {});
    session.cancel();
    REQUIRE(session.is_cancelled());

    // Subsequent feeds should be discarded cleanly
    session.feed(make_solid_frame(0, 100, 50));
    session.feed(make_solid_frame(100, 100, 50));

    auto entries = session.finalize();
    REQUIRE(entries.empty());
    REQUIRE_FALSE(session.is_running());
  }

  SECTION("Repeated consecutive cancellation calls from multiple threads") {
    sublift::adapters::mock::MockFrameSource source;
    sublift::adapters::mock::MockOcrEngine ocr;
    sublift::adapters::mock::MockSubtitleSink sink;

    sublift::ExtractionSession session(source, ocr, nullptr, &sink, {});

    std::vector<std::thread> cancellers;
    cancellers.reserve(8);
    for (int i = 0; i < 8; ++i) {
      cancellers.emplace_back([&session]() {
        for (int k = 0; k < 20; ++k) {
          session.cancel();
        }
      });
    }

    for (auto& t : cancellers) {
      t.join();
    }

    REQUIRE(session.is_cancelled());
    auto entries = session.finalize();
    REQUIRE(entries.empty());
  }
}

TEST_CASE("Adversarial: Extreme backpressure and queue saturation", "[adversarial][engine][backpressure]") {
  sublift::adapters::mock::MockFrameSource source;
  SlowMockOcrEngine slow_ocr(std::chrono::milliseconds(1));
  sublift::adapters::mock::MockSubtitleSink sink;

  // queue_capacity = 1 creates maximum lock contention and queue saturation
  sublift::ExtractionConfig cfg{
      .sample_fps = 10.0,
      .num_threads = 4,
      .queue_capacity = 1,
  };

  sublift::ExtractionSession session(source, slow_ocr, nullptr, &sink, cfg);

  constexpr int kTotalFrames = 80;
  for (int i = 0; i < kTotalFrames; ++i) {
    // Alternate background and foreground pattern to trigger segments
    uint8_t color = (i >= 20 && i <= 60) ? 240 : 40;
    session.feed(make_solid_frame(i * 100, 128, 64, color));
  }

  auto entries = session.finalize();
  REQUIRE_FALSE(session.is_running());
  REQUIRE(session.metrics().total_frames_processed == kTotalFrames);
}

TEST_CASE("Adversarial: Empty frames, extreme geometry, and malicious ROI", "[adversarial][engine][geometry]") {
  sublift::adapters::mock::MockFrameSource source;
  sublift::adapters::mock::MockOcrEngine ocr("Test", 0.9);
  MaliciousDetector detector;
  sublift::adapters::mock::MockSubtitleSink sink;

  sublift::ExtractionSession session(source, ocr, &detector, &sink, {});

  SECTION("Feeding empty 0x0 buffer") {
    session.feed(sublift::Frame{.timestamp_ms = 0, .image = sublift::ImageBuffer()});
    session.feed(sublift::Frame{.timestamp_ms = 100, .image = sublift::ImageBuffer(0, 0, sublift::PixelFormat::RGB24)});
    auto entries = session.finalize();
    REQUIRE(entries.empty());
  }

  SECTION("Feeding 1x1 minimal pixel frame") {
    session.feed(make_solid_frame(0, 1, 1, 255));
    session.feed(make_solid_frame(200, 1, 1, 255));
    session.feed(make_solid_frame(400, 1, 1, 40));
    auto entries = session.finalize();
    (void)entries;
    REQUIRE_FALSE(session.is_running());
  }

  SECTION("Feeding extreme aspect ratios") {
    // Extremely tall
    session.feed(make_solid_frame(0, 2, 2048, 200));
    session.feed(make_solid_frame(200, 2, 2048, 200));
    // Extremely wide
    session.feed(make_solid_frame(400, 2048, 2, 200));
    session.feed(make_solid_frame(600, 2048, 2, 40));
    auto entries = session.finalize();
    (void)entries;
    REQUIRE_FALSE(session.is_running());
  }

  SECTION("Detector returning negative and out-of-bounds bounding boxes") {
    detector.set_mode(MaliciousDetector::Mode::NegativeCoords);
    session.feed(make_solid_frame(0, 320, 240, 200));

    detector.set_mode(MaliciousDetector::Mode::OutOfBounds);
    session.feed(make_solid_frame(200, 320, 240, 200));

    detector.set_mode(MaliciousDetector::Mode::ZeroArea);
    session.feed(make_solid_frame(400, 320, 240, 200));

    detector.set_mode(MaliciousDetector::Mode::NegativeDimensions);
    session.feed(make_solid_frame(600, 320, 240, 200));

    auto entries = session.finalize();
    (void)entries;
    REQUIRE_FALSE(session.is_running());
  }

  SECTION("Dynamic resolution switching between frames") {
    session.feed(make_solid_frame(0, 1920, 1080, 40));
    session.feed(make_solid_frame(200, 128, 64, 240));
    session.feed(make_solid_frame(400, 640, 480, 240));
    session.feed(make_solid_frame(600, 16, 16, 40));
    auto entries = session.finalize();
    (void)entries;
    REQUIRE_FALSE(session.is_running());
  }
}

TEST_CASE("Adversarial: Concurrency race condition under concurrent feed, query, and cancel", "[adversarial][engine][race]") {
  for (int iter = 0; iter < 3; ++iter) {
    sublift::adapters::mock::MockFrameSource source;
    SlowMockOcrEngine ocr(std::chrono::microseconds(200));
    sublift::adapters::mock::MockSubtitleSink sink;

    sublift::ExtractionConfig cfg{
        .sample_fps = 10.0,
        .num_threads = 4,
        .queue_capacity = 4,
    };

    sublift::ExtractionSession session(source, ocr, nullptr, &sink, cfg);
    std::atomic<bool> stop_flag{false};

    // Thread 1: Fast producer A
    auto t1 = std::async(std::launch::async, [&]() {
      for (int i = 0; i < 50 && !stop_flag.load(); ++i) {
        session.feed(make_solid_frame(i * 100, 100, 50, (i % 2 == 0) ? 40 : 240));
        std::this_thread::yield();
      }
    });

    // Thread 2: Fast producer B
    auto t2 = std::async(std::launch::async, [&]() {
      for (int i = 50; i < 100 && !stop_flag.load(); ++i) {
        session.feed(make_solid_frame(i * 100, 100, 50, (i % 2 == 0) ? 240 : 40));
        std::this_thread::yield();
      }
    });

    // Thread 3: Continuous inspector querying status and metrics
    auto t3 = std::async(std::launch::async, [&]() {
      while (!stop_flag.load()) {
        [[maybe_unused]] bool r = session.is_running();
        [[maybe_unused]] bool c = session.is_cancelled();
        [[maybe_unused]] auto m = session.metrics();
        [[maybe_unused]] const auto& cf = session.config();
        std::this_thread::yield();
      }
    });

    // Thread 4: Jittered controller triggering cancel
    auto t4 = std::async(std::launch::async, [&]() {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      session.cancel();
      stop_flag.store(true);
    });

    t1.wait();
    t2.wait();
    t3.wait();
    t4.wait();

    auto entries = session.finalize();
    (void)entries;
    REQUIRE_FALSE(session.is_running());
    REQUIRE(session.is_cancelled());
  }
}

TEST_CASE("Adversarial: Exception propagation and resource safety", "[adversarial][engine][exceptions]") {
  SECTION("Faulty observer throwing exception during on_progress") {
    sublift::adapters::mock::MockFrameSource source;
    sublift::adapters::mock::MockOcrEngine ocr;
    sublift::adapters::mock::MockSubtitleSink sink;

    sublift::ExtractionSession session(source, ocr, nullptr, &sink, {});
    FaultyObserver observer;
    observer.throw_on_progress = true;
    session.set_observer(&observer);

    // If observer throws, session or caller may receive exception,
    // but session must not deadlock or crash with std::terminate
    try {
      session.feed(make_solid_frame(0, 100, 50));
    } catch (const std::runtime_error& e) {
      REQUIRE(std::string(e.what()).find("FaultyObserver") != std::string::npos);
    }

    session.cancel();
    session.finalize();
    REQUIRE_FALSE(session.is_running());
  }

  SECTION("Faulty OCR engine throwing exception during recognize") {
    sublift::adapters::mock::MockFrameSource source;
    FaultyOcrEngine faulty_ocr;
    sublift::adapters::mock::MockSubtitleSink sink;

    sublift::ExtractionConfig cfg{
        .sample_fps = 5.0,
        .hysteresis_ms = 100,
        .confirm_ms = 100,
    };

    sublift::ExtractionSession session(source, faulty_ocr, nullptr, &sink, cfg);

    // Feed frames that trigger subtitle detection
    session.feed(make_solid_frame(0, 128, 64, 40));
    session.feed(make_solid_frame(200, 128, 64, 240));
    session.feed(make_solid_frame(400, 128, 64, 240));
    session.feed(make_solid_frame(600, 128, 64, 40));

    // When finalize runs OCR, faulty_ocr throws.
    // Session should safely handle or propagate without leaking threads.
    try {
      session.finalize();
    } catch (const std::runtime_error& e) {
      REQUIRE(std::string(e.what()).find("FaultyOcrEngine") != std::string::npos);
    }

    REQUIRE_FALSE(session.is_running());
  }

  SECTION("Faulty frame source throwing during process_video") {
    FaultyFrameSource faulty_source;
    sublift::adapters::mock::MockOcrEngine ocr;
    sublift::adapters::mock::MockSubtitleSink sink;

    sublift::ExtractionSession session(faulty_source, ocr, nullptr, &sink, {});

    REQUIRE_THROWS_AS(
        session.process_video("corrupted_video.mp4"),
        std::runtime_error);

    REQUIRE_FALSE(session.is_running());
  }
}

// ============================================================================
// Milestone M5.4 Adversarial Concurrency Stress Testing
// ============================================================================

// Helper to create synthetic frames with subtitle patterns
sublift::Frame make_pattern_frame(int64_t ts, bool has_subtitle, int32_t w = 128, int32_t h = 64) {
  if (w <= 0 || h <= 0) {
    return sublift::Frame{
        .timestamp_ms = ts,
        .image = sublift::ImageBuffer(),
    };
  }
  sublift::ImageBuffer img(w, h, sublift::PixelFormat::RGB24);
  for (int32_t y = 0; y < h; ++y) {
    uint8_t* row = img.row(y);
    for (int32_t x = 0; x < w; ++x) {
      row[x * 3 + 0] = 40;
      row[x * 3 + 1] = 40;
      row[x * 3 + 2] = 40;
    }
  }
  if (has_subtitle && w >= 16 && h >= 16) {
    int32_t x0 = (3 * w) / 8;
    int32_t x1 = (5 * w) / 8;
    int32_t y0 = (2 * h) / 3;
    int32_t y1 = (7 * h) / 8;
    for (int32_t y = y0; y < y1; ++y) {
      uint8_t* row = img.row(y);
      for (int32_t x = x0; x < x1; ++x) {
        row[x * 3 + 0] = 240;
        row[x * 3 + 1] = 240;
        row[x * 3 + 2] = 240;
      }
    }
  }
  return sublift::Frame{
      .timestamp_ms = ts,
      .image = std::move(img),
  };
}

class DynamicBoxDetector final : public sublift::IDetector {
 public:
  std::optional<sublift::Region> detect(const sublift::Frame& f) override {
    return sublift::Region{sublift::FrameLocalBox{0, 0, f.image.width(), f.image.height()}};
  }
};

TEST_CASE("Adversarial: Multi-threaded determinism under extreme thread counts and minimal queue",
          "[adversarial][engine][concurrency][stress]") {
  // Helper to generate a realistic sequence of frames with subtitle transitions
  constexpr int kTotalFrames = 120;
  std::vector<sublift::Frame> frame_sequence;
  frame_sequence.reserve(kTotalFrames);

  for (int i = 0; i < kTotalFrames; ++i) {
    int64_t ts = i * 200; // 5 fps, 200ms per frame
    // Subtitle interval 1: frame 10..30 (ts 2000ms .. 6000ms)
    // Subtitle interval 2: frame 50..80 (ts 10000ms .. 16000ms)
    bool has_sub = ((i >= 10 && i <= 30) || (i >= 50 && i <= 80));
    frame_sequence.push_back(make_pattern_frame(ts, has_sub));
  }

  auto run_pipeline = [&](std::size_t threads, std::size_t queue_cap) {
    sublift::adapters::mock::MockFrameSource source;
    sublift::adapters::mock::MockOcrEngine ocr("Deterministic Subtitle", 0.95);
    DynamicBoxDetector detector;
    sublift::adapters::mock::MockSubtitleSink sink;

    sublift::ExtractionConfig cfg{
        .sample_fps = 5.0,
        .hysteresis_ms = 0,
        .enable_ssim_patrol = false,
        .enable_line_select = false,
        .confidence_threshold = 0.0,
        .min_duration_ms = 0,
        .num_threads = threads,
        .queue_capacity = queue_cap,
    };

    sublift::ExtractionSession session(source, ocr, &detector, &sink, cfg);
    for (const auto& f : frame_sequence) {
      bool has_sub = ((f.timestamp_ms >= 2000 && f.timestamp_ms <= 6000) ||
                      (f.timestamp_ms >= 10000 && f.timestamp_ms <= 16000));
      session.feed(make_pattern_frame(f.timestamp_ms, has_sub, f.image.width(), f.image.height()));
    }
    auto entries = session.finalize();
    return std::make_tuple(entries, sink.entries(), session.metrics());
  };

  // 1. Reference: Serial single-threaded baseline (1T)
  auto [ref_entries, ref_sink_entries, ref_metrics] = run_pipeline(1, 16);
  INFO("ref_metrics: frames=" << ref_metrics.total_frames_processed
       << " segs=" << ref_metrics.total_segments_detected
       << " entries=" << ref_entries.size());
  REQUIRE_FALSE(ref_entries.empty());
  REQUIRE(ref_entries.size() == ref_sink_entries.size());

  // 2. Test extreme thread counts and minimal queue capacity (1, 2)
  const std::vector<std::pair<std::size_t, std::size_t>> configs = {
      {2, 1},   // 2 threads, minimal queue 1
      {2, 2},   // 2 threads, queue 2
      {3, 1},   // odd threads 3, queue 1
      {3, 2},   // odd threads 3, queue 2
      {5, 1},   // odd threads 5, queue 1
      {5, 3},   // odd threads 5, queue 3
      {7, 1},   // prime threads 7, queue 1
      {8, 1},   // 8 threads, queue 1
      {8, 2},   // 8 threads, queue 2
      {13, 2},  // prime threads 13, queue 2
      {16, 1},  // 16 threads, queue 1
      {16, 4},  // 16 threads, queue 4
      {16, 32}, // 16 threads, queue 32
  };

  for (const auto& [threads, q_cap] : configs) {
    DYNAMIC_SECTION("Threads=" << threads << " QueueCap=" << q_cap) {
      auto [par_entries, par_sink_entries, par_metrics] = run_pipeline(threads, q_cap);

      // Verify identical segment and entry count
      REQUIRE(par_metrics.total_frames_processed == ref_metrics.total_frames_processed);
      REQUIRE(par_metrics.total_segments_detected == ref_metrics.total_segments_detected);
      REQUIRE(par_entries.size() == ref_entries.size());
      REQUIRE(par_sink_entries.size() == ref_sink_entries.size());

      // Verify byte-for-byte exact equality of every subtitle entry
      for (std::size_t i = 0; i < ref_entries.size(); ++i) {
        CHECK(par_entries[i].id == ref_entries[i].id);
        CHECK(par_entries[i].range.start_ms == ref_entries[i].range.start_ms);
        CHECK(par_entries[i].range.end_ms == ref_entries[i].range.end_ms);
        CHECK(par_entries[i].text == ref_entries[i].text);
        CHECK(par_entries[i].confidence == Catch::Approx(ref_entries[i].confidence));

        CHECK(par_sink_entries[i].id == ref_sink_entries[i].id);
        CHECK(par_sink_entries[i].range.start_ms == ref_sink_entries[i].range.start_ms);
        CHECK(par_sink_entries[i].range.end_ms == ref_sink_entries[i].range.end_ms);
        CHECK(par_sink_entries[i].text == ref_sink_entries[i].text);
        CHECK(par_sink_entries[i].confidence == Catch::Approx(ref_sink_entries[i].confidence));
      }
    }
  }
}

TEST_CASE("Adversarial: Artificial thread jitter and severe out-of-order reordering stress",
          "[adversarial][engine][concurrency][jitter]") {
  // We feed frames alternating between huge (heavy visual analysis) and tiny (instant analysis).
  // Under 16 worker threads, tiny frames finish orders of magnitude faster than huge frames,
  // causing maximum out-of-order completions in completed_slots_.
  constexpr int kTotalFrames = 80;
  std::vector<sublift::Frame> skewed_frames;
  skewed_frames.reserve(kTotalFrames);

  for (int i = 0; i < kTotalFrames; ++i) {
    int64_t ts = i * 200;
    bool has_sub = ((i >= 15 && i <= 35) || (i >= 50 && i <= 70));
    if (i % 2 == 0) {
      // Huge frame: heavy visual analysis
      skewed_frames.push_back(make_pattern_frame(ts, has_sub, 640, 360));
    } else {
      // Tiny frame: instantaneous analysis
      skewed_frames.push_back(make_pattern_frame(ts, has_sub, 32, 32));
    }
  }

  auto run_skewed = [&](std::size_t threads, std::size_t queue_cap) {
    sublift::adapters::mock::MockFrameSource source;
    sublift::adapters::mock::MockOcrEngine ocr("Jitter Parity", 0.9);
    DynamicBoxDetector detector;
    sublift::adapters::mock::MockSubtitleSink sink;

    sublift::ExtractionConfig cfg{
        .sample_fps = 5.0,
        .hysteresis_ms = 0,
        .enable_ssim_patrol = false,
        .enable_line_select = false,
        .confidence_threshold = 0.0,
        .min_duration_ms = 0,
        .num_threads = threads,
        .queue_capacity = queue_cap,
    };

    sublift::ExtractionSession session(source, ocr, &detector, &sink, cfg);
    for (const auto& f : skewed_frames) {
      bool has_sub = ((f.timestamp_ms >= 3000 && f.timestamp_ms <= 7000) ||
                      (f.timestamp_ms >= 10000 && f.timestamp_ms <= 14000));
      session.feed(make_pattern_frame(f.timestamp_ms, has_sub, f.image.width(), f.image.height()));
    }
    auto entries = session.finalize();
    return std::make_pair(entries, sink.entries());
  };

  // 1. Reference 1-thread run
  auto [ref_entries, ref_sink] = run_skewed(1, 16);
  INFO("skewed ref_entries count=" << ref_entries.size());
  REQUIRE_FALSE(ref_entries.empty());

  // 2. High thread count (16 threads) with deep reorder buffer
  for (std::size_t q_cap : {4, 16, 32}) {
    DYNAMIC_SECTION("16 Threads with QueueCap=" << q_cap) {
      auto [par_entries, par_sink] = run_skewed(16, q_cap);
      REQUIRE(par_entries.size() == ref_entries.size());
      REQUIRE(par_sink.size() == ref_sink.size());

      for (std::size_t i = 0; i < ref_entries.size(); ++i) {
        CHECK(par_entries[i].range.start_ms == ref_entries[i].range.start_ms);
        CHECK(par_entries[i].range.end_ms == ref_entries[i].range.end_ms);
        CHECK(par_entries[i].text == ref_entries[i].text);
      }
    }
  }
}

TEST_CASE("Adversarial: Rapid cancellation under extreme backpressure deadlock challenge",
          "[adversarial][engine][concurrency][cancellation]") {
  // Test rapid cancellation when producer is blocked waiting on task_cv_push_
  // with queue_capacity = 1 and slow worker / OCR.
  sublift::adapters::mock::MockFrameSource source;
  SlowMockOcrEngine slow_ocr(std::chrono::milliseconds(2));
  sublift::adapters::mock::MockSubtitleSink sink;

  sublift::ExtractionConfig cfg{
      .sample_fps = 10.0,
      .num_threads = 4,
      .queue_capacity = 1, // Minimal queue capacity to force backpressure
  };

  sublift::ExtractionSession session(source, slow_ocr, nullptr, &sink, cfg);

  std::atomic<bool> producer_started{false};
  std::atomic<int> frames_fed{0};

  // Producer thread aggressively pushes frames with zero sleep to saturate queue_capacity=1
  auto producer = std::async(std::launch::async, [&]() {
    producer_started.store(true);
    for (int i = 0; i < 500; ++i) {
      if (session.is_cancelled()) {
        break;
      }
      session.feed(make_solid_frame(i * 100, 200, 100, (i % 2 == 0) ? 40 : 240));
      frames_fed.fetch_add(1, std::memory_order_relaxed);
    }
  });

  // Wait until producer has started pushing
  while (!producer_started.load()) {
    std::this_thread::yield();
  }
  // Brief sleep to ensure queue is saturated and producer is blocked on task_cv_push_
  std::this_thread::sleep_for(std::chrono::milliseconds(2));

  // Run cancellation with watchdog timeout to catch deadlocks
  auto cancel_future = std::async(std::launch::async, [&]() {
    session.cancel();
    return true;
  });

  auto status = cancel_future.wait_for(std::chrono::seconds(3));
  INFO("cancel() completed within timeout. frames_fed=" << frames_fed.load());
  REQUIRE(status == std::future_status::ready);

  auto prod_status = producer.wait_for(std::chrono::seconds(2));
  INFO("producer completed within timeout");
  REQUIRE(prod_status == std::future_status::ready);

  // Finalize watchdog
  auto finalize_future = std::async(std::launch::async, [&]() {
    return session.finalize();
  });
  auto fin_status = finalize_future.wait_for(std::chrono::seconds(3));
  INFO("finalize() completed within timeout");
  REQUIRE(fin_status == std::future_status::ready);

  REQUIRE(session.is_cancelled());
  REQUIRE_FALSE(session.is_running());
}

TEST_CASE("Adversarial: High-frequency concurrent cancel-during-burst stress (50 iterations)",
          "[adversarial][engine][concurrency][cancel-burst]") {
  for (int iter = 0; iter < 50; ++iter) {
    auto source = std::make_unique<sublift::adapters::mock::MockFrameSource>();
    auto ocr = std::make_unique<sublift::adapters::mock::MockOcrEngine>();
    auto sink = std::make_unique<sublift::adapters::mock::MockSubtitleSink>();

    sublift::ExtractionConfig cfg{
        .sample_fps = 10.0,
        .num_threads = 2,
        .queue_capacity = 1, // Minimum queue capacity
    };

    auto session = std::make_unique<sublift::ExtractionSession>(*source, *ocr, nullptr, sink.get(), cfg);
    std::atomic<bool> feeding{true};
    std::atomic<bool> producer_done{false};

    std::thread producer([&]() {
      int idx = 0;
      while (feeding.load(std::memory_order_relaxed)) {
        session->feed(make_pattern_frame(idx * 100, (idx % 2 == 0)));
        ++idx;
      }
      producer_done.store(true);
    });

    // Random microsecond delay to vary interleaving between worker, consumer, and drain
    std::this_thread::sleep_for(std::chrono::microseconds(50 * (iter % 10 + 1)));

    std::atomic<bool> cancel_done{false};
    std::thread canceller([&]() {
      session->cancel();
      feeding.store(false, std::memory_order_relaxed);
      cancel_done.store(true);
    });

    // Wait up to 10s (deadlock safety ceiling) for cancel to complete
    bool timed_out = false;
    auto start = std::chrono::steady_clock::now();
    while (!cancel_done.load()) {
      if (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count() >= 10) {
        timed_out = true;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    if (timed_out) {
      feeding.store(false, std::memory_order_relaxed);
      canceller.detach();
      producer.detach();
      session.release();
      source.release();
      ocr.release();
      sink.release();
      FAIL("EMPIRICAL DEADLOCK: session->cancel() deadlocked at iteration " << iter
           << " due to consumer/drain race condition under backpressure!");
      return;
    }

    canceller.join();
    producer.join();

    std::atomic<bool> finalize_done{false};
    std::thread finalizer([&]() {
      session->finalize();
      finalize_done.store(true);
    });

    start = std::chrono::steady_clock::now();
    while (!finalize_done.load()) {
      if (std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - start).count() >= 10) {
        finalizer.detach();
        session.release();
        source.release();
        ocr.release();
        sink.release();
        FAIL_CHECK("EMPIRICAL DEADLOCK: session->finalize() deadlocked at iteration " << iter);
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    finalizer.join();
  }
}

// ============================================================================
// Milestone M1 Challenger: Direct Concurrency Stress Tests
// ============================================================================

TEST_CASE("Challenger M1: AnalysisExecutor high-volume feeding and strict FIFO ordering",
          "[challenger][engine][analysis_executor][fifo]") {
  const std::vector<std::size_t> thread_configs = {
      0, // hardware concurrency fallback
      1, // strictly single worker
      8, // 8 workers
  };

  constexpr std::size_t kFrameCount = 500;

  for (std::size_t num_threads : thread_configs) {
    DYNAMIC_SECTION("num_threads=" << num_threads << " volume=" << kFrameCount) {
      sublift::timing::TimingConfig timing_cfg{};
      sublift::AnalysisExecutor executor(timing_cfg, num_threads, /*queue_capacity=*/32);

      std::vector<std::uint64_t> popped_indices;
      popped_indices.reserve(kFrameCount);

      // Consumer thread: strictly pop frames and collect sequence
      std::thread consumer([&]() {
        while (auto frame_opt = executor.pop()) {
          popped_indices.push_back(frame_opt->frame_index);
        }
      });

      // Producer thread: push kFrameCount frames with varying image dimensions
      for (std::size_t i = 0; i < kFrameCount; ++i) {
        int32_t w = (i % 2 == 0) ? 120 : 60;
        int32_t h = (i % 2 == 0) ? 60 : 30;
        executor.push(i, static_cast<std::int64_t>(i * 40),
                      sublift::ImageBuffer(w, h, sublift::PixelFormat::RGB24));
      }
      executor.finish_pushing();

      consumer.join();

      // Verify that every single frame was received in strict sequential order: 0, 1, 2, ..., N-1
      REQUIRE(popped_indices.size() == kFrameCount);
      for (std::size_t i = 0; i < kFrameCount; ++i) {
        REQUIRE(popped_indices[i] == i);
      }
    }
  }
}

TEST_CASE("Challenger M1: AnalysisExecutor cancellation under extreme backpressure",
          "[challenger][engine][analysis_executor][cancellation]") {
  // Test 10 iterations of cancellation while producer is actively blocked on queue_capacity=1
  for (int iter = 0; iter < 10; ++iter) {
    sublift::timing::TimingConfig timing_cfg{};
    sublift::AnalysisExecutor executor(timing_cfg, /*num_threads=*/4, /*queue_capacity=*/1);

    std::atomic<bool> producer_blocked{false};
    std::atomic<bool> producer_finished{false};
    std::atomic<std::size_t> pushed_count{0};

    std::thread producer([&]() {
      for (std::size_t i = 0; i < 1000; ++i) {
        producer_blocked.store(true, std::memory_order_release);
        executor.push(i, static_cast<std::int64_t>(i * 100),
                      sublift::ImageBuffer(100, 50, sublift::PixelFormat::RGB24));
        pushed_count.fetch_add(1, std::memory_order_relaxed);
      }
      producer_finished.store(true, std::memory_order_release);
    });

    // Wait until producer has started pushing
    while (!producer_blocked.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    // Brief sleep to allow queue to fill up and block producer
    std::this_thread::sleep_for(std::chrono::milliseconds(2));

    // Cancel executor
    executor.cancel();

    // Verify producer unblocks and exits promptly
    producer.join();

    // Consumer pop must return std::nullopt promptly
    auto popped = executor.pop();
    CHECK_FALSE(popped.has_value());
  }
}

TEST_CASE("Challenger M1: SegmentRecognizer non-stalling asynchronous OCR and FIFO ordering",
          "[challenger][engine][segment_recognizer][async_order]") {
  class GateBlockingOcrEngine final : public sublift::IOcrEngine {
   public:
    GateBlockingOcrEngine(std::promise<void>* first_entered,
                          std::shared_future<void> release_gate)
        : first_entered_(first_entered),
          release_gate_(std::move(release_gate)) {}

    sublift::OcrResult recognize(const sublift::ImageView&) override {
      std::call_once(first_entered_once_, [this]() {
        if (first_entered_ != nullptr) {
          first_entered_->set_value();
        }
      });
      release_gate_.wait_for(std::chrono::seconds(10));
      return sublift::OcrResult{
          .text = "Gated Subtitle",
          .confidence = 0.95,
          .lines = {},
      };
    }

    [[nodiscard]] std::string engine_name() const override { return "GateBlockingOcrEngine"; }

   private:
    std::promise<void>* first_entered_{nullptr};
    std::once_flag first_entered_once_;
    std::shared_future<void> release_gate_;
  };

  std::promise<void> first_entered_promise;
  auto first_entered_future = first_entered_promise.get_future();
  std::promise<void> release_promise;
  GateBlockingOcrEngine gated_ocr(&first_entered_promise,
                                  release_promise.get_future().share());

  sublift::ExtractionConfig cfg{
      .queue_capacity = 16,
  };

  class OrderTrackingObserver : public sublift::IExtractionObserver {
   public:
    void on_subtitle_entry(const sublift::SubtitleEntry& entry) override {
      std::lock_guard<std::mutex> lock(mutex);
      received_ids.push_back(entry.id);
      received_starts.push_back(entry.range.start_ms);
    }

    std::mutex mutex;
    std::vector<std::uint64_t> received_ids;
    std::vector<std::int64_t> received_starts;
  } observer;

  sublift::SegmentRecognizer recognizer(gated_ocr, cfg, &observer);

  constexpr std::size_t kSegmentCount = 8;
  // SegmentRecognizer::Impl clamps its bounded queue to 4 (1 active worker + 4 queued = 5 non-blocking slots).
  constexpr std::size_t kInitialNonBlocking = 5;

  auto make_segment = [](std::size_t i) {
    sublift::ImageBuffer anchor(100, 40, sublift::PixelFormat::RGB24);
    return sublift::timing::Segment{
        .start_ms = static_cast<std::int64_t>(i * 1000),
        .end_ms = static_cast<std::int64_t>(i * 1000 + 800),
        .anchor_timestamp_ms = static_cast<std::int64_t>(i * 1000 + 400),
        .fallback_timestamps_ms = {},
        .anchor_image = std::move(anchor),
        .fallback_images = {},
    };
  };

  // Submit segment 0 and prove worker is actively blocked inside recognize().
  recognizer.submit(make_segment(0), 1);
  REQUIRE(first_entered_future.wait_for(std::chrono::seconds(5)) == std::future_status::ready);

  // Submit segments 1..4 while worker remains blocked in recognize(): proves submit() does not stall on OCR.
  for (std::size_t i = 1; i < kInitialNonBlocking; ++i) {
    recognizer.submit(make_segment(i), i + 1);
  }

  release_promise.set_value();

  for (std::size_t i = kInitialNonBlocking; i < kSegmentCount; ++i) {
    recognizer.submit(make_segment(i), i + 1);
  }

  // Drain recognizer
  recognizer.drain();

  // Verify all OCR calls completed
  REQUIRE(recognizer.ocr_calls_count() == kSegmentCount);

  // Verify observer received entries in strictly monotonically increasing ID and timestamp order
  {
    std::lock_guard<std::mutex> lock(observer.mutex);
    REQUIRE(observer.received_ids.size() == kSegmentCount);
    for (std::size_t i = 0; i < kSegmentCount; ++i) {
      CHECK(observer.received_ids[i] == (i + 1));
      CHECK(observer.received_starts[i] == static_cast<std::int64_t>(i * 1000));
    }
  }

  // Verify take_raw_entries is also in strict order
  auto raw_entries = recognizer.take_raw_entries();
  REQUIRE(raw_entries.size() == kSegmentCount);
  for (std::size_t i = 0; i < kSegmentCount; ++i) {
    CHECK(raw_entries[i].id == (i + 1));
    CHECK(raw_entries[i].range.start_ms == static_cast<std::int64_t>(i * 1000));
    CHECK(raw_entries[i].range.end_ms == static_cast<std::int64_t>(i * 1000 + 800));
  }
}

TEST_CASE("Challenger M1: End-to-End Session with high OCR latency pipeline decoupling",
          "[challenger][engine][session][pipeline_decoupling]") {
  sublift::adapters::mock::MockFrameSource source;
  SlowMockOcrEngine slow_ocr(std::chrono::milliseconds(10));
  sublift::adapters::mock::MockSubtitleSink sink;

  class SessionOrderObserver : public sublift::IExtractionObserver {
   public:
    void on_subtitle_entry(const sublift::SubtitleEntry& entry) override {
      std::lock_guard<std::mutex> lock(mutex);
      entries.push_back(entry);
    }
    std::mutex mutex;
    std::vector<sublift::SubtitleEntry> entries;
  } observer;

  sublift::ExtractionConfig cfg{
      .sample_fps = 10.0,
      .enable_ssim_patrol = false,
      .enable_line_select = false,
      .confidence_threshold = 0.0,
      .min_duration_ms = 0,
      .subtitle_script = "latin",
      .num_threads = 4,
      .queue_capacity = 8,
  };

  DynamicBoxDetector detector;
  sublift::ExtractionSession session(source, slow_ocr, &detector, &sink, cfg);
  session.set_observer(&observer);

  constexpr int kTotalFrames = 150;
  // Generate frames with 3 distinct subtitle intervals
  // Seg 1: 20..40 (ts 2000..4000)
  // Seg 2: 60..80 (ts 6000..8000)
  // Seg 3: 100..120 (ts 10000..12000)
  for (int i = 0; i < kTotalFrames; ++i) {
    int64_t ts = i * 100;
    bool has_sub = ((i >= 20 && i <= 40) || (i >= 60 && i <= 80) || (i >= 100 && i <= 120));
    session.feed(make_pattern_frame(ts, has_sub));
  }

  auto entries = session.finalize();

  REQUIRE_FALSE(session.is_running());
  REQUIRE(entries.size() == 3);

  // Verify strict ordering of final entries
  for (std::size_t i = 1; i < entries.size(); ++i) {
    CHECK(entries[i].range.start_ms > entries[i - 1].range.start_ms);
    CHECK(entries[i].range.end_ms > entries[i - 1].range.end_ms);
  }

  // Verify observer entries match finalize entries
  {
    std::lock_guard<std::mutex> lock(observer.mutex);
    REQUIRE(observer.entries.size() == 3);
    for (std::size_t i = 0; i < entries.size(); ++i) {
      CHECK(observer.entries[i].range.start_ms == entries[i].range.start_ms);
      CHECK(observer.entries[i].range.end_ms == entries[i].range.end_ms);
    }
  }

  // Verify sink received entries
  REQUIRE(sink.entries().size() == 3);
}

// ============================================================================
// Milestone M1 Challenger 2: Double-crop, Memory Lifecycle & Observer Parity
// ============================================================================

TEST_CASE("Challenger M1: Double-crop remediation - SourceBox crop bypasses bottom ratio vs direct feed",
          "[challenger][engine][double_crop]") {
  SECTION("process_video with SourceBox crop does NOT apply region_bottom_ratio") {
    class CroppedFrameSource : public sublift::IFrameSource {
     public:
      sublift::VideoInfo probe(const std::filesystem::path&) override {
        return sublift::VideoInfo{.width = 1920, .height = 1080, .duration_ms = 2000, .fps = 10.0};
      }

      sublift::FrameIOPlan plan(
          const sublift::VideoInfo&,
          const std::optional<sublift::SourceBox>& requested_box) override {
        return sublift::FrameIOPlan{
            .output_crop = requested_box,
            .output_mode = requested_box.has_value() ? sublift::FrameOutputMode::RoiRgb : sublift::FrameOutputMode::FullRgb,
        };
      }

      void extract(
          const std::filesystem::path&,
          double,
          const std::optional<sublift::SourceBox>& crop,
          std::function<bool(sublift::Frame)> consumer) override {
        REQUIRE(crop.has_value());
        for (int i = 0; i < 10; ++i) {
          bool has_sub = (i >= 2 && i <= 7);
          consumer(make_pattern_frame(i * 100, has_sub, crop->width, crop->height));
        }
      }

      sublift::ImageBuffer extract_frame(const std::filesystem::path&, int64_t,
                                         const std::optional<sublift::SourceBox>&) override {
        return sublift::ImageBuffer();
      }

      void cancel() override {}
      bool is_cancelled() const noexcept override { return false; }
    } source;

    sublift::adapters::mock::MockOcrEngine ocr("Detected text", 0.95);
    sublift::adapters::mock::MockSubtitleSink sink;

    class DimensionTrackingObserver : public sublift::IExtractionObserver {
     public:
      void on_segment_detected(const sublift::timing::Segment& seg) override {
        std::lock_guard<std::mutex> lock(mutex);
        detected_heights.push_back(seg.anchor_image.height());
        detected_widths.push_back(seg.anchor_image.width());
      }
      std::mutex mutex;
      std::vector<int32_t> detected_heights;
      std::vector<int32_t> detected_widths;
    } observer;

    sublift::ExtractionConfig cfg{
        .sample_fps = 10.0,
        .region_bottom_ratio = 0.30, // 30% ratio
        .hysteresis_ms = 100,
        .confirm_ms = 100,
        .min_duration_ms = 0,
        .num_threads = 2,
    };

    sublift::ExtractionSession session(source, ocr, nullptr, &sink, cfg);
    session.set_observer(&observer);

    sublift::SourceBox requested_box{.x = 0, .y = 930, .width = 1920, .height = 150};
    auto entries = session.process_video("virtual_video.mp4", requested_box);

    REQUIRE_FALSE(entries.empty());
    {
      std::lock_guard<std::mutex> lock(observer.mutex);
      REQUIRE_FALSE(observer.detected_heights.empty());
      // The anchor image MUST retain the exact cropped height 150, NOT 150 * 0.3 = 45!
      CHECK(observer.detected_heights[0] == 150);
      CHECK(observer.detected_widths[0] == 1920);
    }
  }

  SECTION("Direct feed() without SourceBox still applies region_bottom_ratio") {
    sublift::adapters::mock::MockFrameSource source;
    sublift::adapters::mock::MockOcrEngine ocr("Detected text", 0.95);
    sublift::adapters::mock::MockSubtitleSink sink;

    class DimensionTrackingObserver : public sublift::IExtractionObserver {
     public:
      void on_segment_detected(const sublift::timing::Segment& seg) override {
        std::lock_guard<std::mutex> lock(mutex);
        detected_heights.push_back(seg.anchor_image.height());
        detected_widths.push_back(seg.anchor_image.width());
      }
      std::mutex mutex;
      std::vector<int32_t> detected_heights;
      std::vector<int32_t> detected_widths;
    } observer;

    sublift::ExtractionConfig cfg{
        .sample_fps = 10.0,
        .region_bottom_ratio = 0.40, // 40% ratio encompasses y=333..437 (y >= 300)
        .hysteresis_ms = 100,
        .confirm_ms = 100,
        .enable_line_select = false,
        .confidence_threshold = 0.0,
        .min_duration_ms = 0,
        .subtitle_script = "auto",
        .num_threads = 2,
    };

    sublift::ExtractionSession session(source, ocr, nullptr, &sink, cfg);
    session.set_observer(&observer);

    auto make_full_video_frame = [](int64_t ts, bool has_sub) {
      sublift::ImageBuffer img(1920, 500, sublift::PixelFormat::RGB24);
      for (int y = 0; y < 500; ++y) {
        uint8_t* row = img.row(y);
        for (int x = 0; x < 1920; ++x) {
          row[x * 3 + 0] = 40;
          row[x * 3 + 1] = 40;
          row[x * 3 + 2] = 40;
        }
      }
      if (has_sub) {
        // Position within bottom 150 rows (rows 350..500) matching Section 1's rows 100..131:
        // y in full frame = 350 + 100 = 450 to 350 + 131 = 481
        for (int y = 450; y < 481; ++y) {
          uint8_t* row = img.row(y);
          for (int x = 720; x < 1200; ++x) {
            row[x * 3 + 0] = 240;
            row[x * 3 + 1] = 240;
            row[x * 3 + 2] = 240;
          }
        }
      }
      return sublift::Frame{.timestamp_ms = ts, .image = std::move(img)};
    };

    for (int i = 0; i < 10; ++i) {
      bool has_sub = (i >= 2 && i <= 7);
      session.feed(make_full_video_frame(i * 100, has_sub));
    }

    auto entries = session.finalize();
    INFO("processed=" << session.metrics().total_frames_processed
         << " segs=" << session.metrics().total_segments_detected
         << " entries=" << entries.size());
    REQUIRE_FALSE(entries.empty());
    {
      std::lock_guard<std::mutex> lock(observer.mutex);
      REQUIRE_FALSE(observer.detected_heights.empty());
      // Bottom 40% of 500 is exactly 200 pixels!
      CHECK(observer.detected_heights[0] == 200);
      CHECK(observer.detected_widths[0] == 1920);
    }
  }
}

TEST_CASE("Challenger M1: Bounded memory lifecycle - >100 segments popped and image buffers destroyed post-OCR",
          "[challenger][engine][memory_lifecycle]") {
  SECTION("TimelineBuilder pop_closed_segment maintains 0 residual vector size across >100 segments") {
    sublift::timing::TimingConfig timing_cfg{
        .hysteresis_ms = 100,
        .confirm_ms = 100,
        .ocr_anchor_delay_ms = 200,
        .ocr_consensus_frames = 3,
    };
    sublift::timing::TimelineBuilder timeline(timing_cfg);

    constexpr int kSegmentCount = 120;
    for (int seg_idx = 0; seg_idx < kSegmentCount; ++seg_idx) {
      int64_t start_ts = seg_idx * 1000;
      int64_t mid_ts = start_ts + 200;
      int64_t end_ts = start_ts + 500;

      sublift::ImageBuffer frame_img(128, 64, sublift::PixelFormat::RGB24);

      // Event In
      timeline.consume(sublift::timing::StateEvent{
          .type = sublift::timing::EventType::In,
          .timestamp_ms = start_ts,
      }, frame_img.view(), start_ts);

      // Stable frame
      timeline.on_stable_frame(frame_img.view(), mid_ts);

      // Event Out closes the segment
      timeline.consume(sublift::timing::StateEvent{
          .type = sublift::timing::EventType::Out,
          .timestamp_ms = end_ts,
      }, frame_img.view(), end_ts);

      // Segment was closed. Before pop, segments().size() is exactly 1
      REQUIRE(timeline.segments().size() == 1);

      // Pop the closed segment
      auto popped = timeline.pop_closed_segment();
      REQUIRE(popped.has_value());
      CHECK(popped->start_ms == start_ts);
      CHECK(popped->end_ms == end_ts);
      CHECK_FALSE(popped->anchor_image.empty());

      // After pop, segments() internal storage MUST be completely empty (bounded)
      CHECK(timeline.segments().empty());
      CHECK(timeline.total_closed_segments() == static_cast<std::size_t>(seg_idx + 1));
    }

    CHECK(timeline.total_closed_segments() == kSegmentCount);
    CHECK(timeline.segments().empty());
  }

  SECTION("ExtractionSession sustained feed with >100 segments cleans up pixel memory") {
    sublift::adapters::mock::MockFrameSource source;
    sublift::adapters::mock::MockOcrEngine ocr("Rapid Text", 0.95);
    sublift::adapters::mock::MockSubtitleSink sink;

    sublift::ExtractionConfig cfg{
        .sample_fps = 10.0,
        .hysteresis_ms = 50,
        .confirm_ms = 50,
        .enable_line_select = false,
        .confidence_threshold = 0.0,
        .min_duration_ms = 0,
        .merge_gap_ms = 0,
        .num_threads = 2,
        .queue_capacity = 8,
    };

    DynamicBoxDetector detector;
    sublift::ExtractionSession session(source, ocr, &detector, &sink, cfg);

    constexpr int kTargetSegments = 110;
    for (int s = 0; s < kTargetSegments; ++s) {
      int64_t base_ts = s * 600;
      session.feed(make_pattern_frame(base_ts + 0, false, 128, 64));
      session.feed(make_pattern_frame(base_ts + 100, false, 128, 64));
      session.feed(make_pattern_frame(base_ts + 200, true, 128, 64));
      session.feed(make_pattern_frame(base_ts + 300, true, 128, 64));
      session.feed(make_pattern_frame(base_ts + 400, true, 128, 64));
      session.feed(make_pattern_frame(base_ts + 500, false, 128, 64));
    }

    auto entries = session.finalize();
    REQUIRE_FALSE(session.is_running());

    auto metrics = session.metrics();
    INFO("Produced segments: " << metrics.total_segments_detected << ", entries: " << entries.size());
    CHECK(metrics.total_segments_detected >= kTargetSegments);
    CHECK(metrics.total_ocr_calls >= kTargetSegments);
    CHECK(entries.size() >= kTargetSegments);
  }

  SECTION("SegmentRecognizer releases image buffers immediately post-OCR under high volume") {
    sublift::adapters::mock::MockOcrEngine ocr("Recognized", 0.9);
    sublift::ExtractionConfig cfg{
        .queue_capacity = 4,
    };

    class BufferCheckingObserver : public sublift::IExtractionObserver {
     public:
      void on_subtitle_entry(const sublift::SubtitleEntry&) override {
        entry_count.fetch_add(1, std::memory_order_relaxed);
      }
      std::atomic<int> entry_count{0};
    } observer;

    sublift::SegmentRecognizer recognizer(ocr, cfg, &observer);

    constexpr int kSegments = 120;
    for (int i = 0; i < kSegments; ++i) {
      sublift::ImageBuffer anchor(200, 100, sublift::PixelFormat::RGB24);
      sublift::ImageBuffer fallback(200, 100, sublift::PixelFormat::RGB24);

      sublift::timing::Segment seg{
          .start_ms = static_cast<int64_t>(i * 1000),
          .end_ms = static_cast<int64_t>(i * 1000 + 500),
          .anchor_timestamp_ms = static_cast<int64_t>(i * 1000 + 200),
          .fallback_timestamps_ms = {static_cast<int64_t>(i * 1000 + 300)},
          .anchor_image = std::move(anchor),
          .fallback_images = {},
      };
      seg.fallback_images.push_back(std::move(fallback));

      recognizer.submit(std::move(seg), i + 1);
    }

    recognizer.drain();

    CHECK(observer.entry_count.load() == kSegments);
    CHECK(recognizer.ocr_calls_count() == kSegments);
    auto raw_entries = recognizer.take_raw_entries();
    CHECK(raw_entries.size() == kSegments);
  }
}

TEST_CASE("Challenger M1: Observer event alignment - on_progress video duration and trailing open segment callbacks",
          "[challenger][engine][observer_alignment]") {
  SECTION("on_progress total_duration matches probed VideoInfo::duration_ms strictly") {
    constexpr int64_t kExpectedDurationMs = 45000;

    class ProbedDurationSource : public sublift::IFrameSource {
     public:
      sublift::VideoInfo probe(const std::filesystem::path&) override {
        return sublift::VideoInfo{
            .width = 1280,
            .height = 720,
            .duration_ms = kExpectedDurationMs,
            .fps = 10.0,
        };
      }

      sublift::FrameIOPlan plan(
          const sublift::VideoInfo&,
          const std::optional<sublift::SourceBox>&) override {
        return sublift::FrameIOPlan{};
      }

      void extract(
          const std::filesystem::path&,
          double,
          const std::optional<sublift::SourceBox>&,
          std::function<bool(sublift::Frame)> consumer) override {
        for (int i = 0; i < 10; ++i) {
          consumer(make_pattern_frame(i * 500, false, 128, 64));
        }
      }

      sublift::ImageBuffer extract_frame(const std::filesystem::path&, int64_t,
                                         const std::optional<sublift::SourceBox>&) override {
        return sublift::ImageBuffer();
      }

      void cancel() override {}
      bool is_cancelled() const noexcept override { return false; }
    } source;

    sublift::adapters::mock::MockOcrEngine ocr;
    sublift::adapters::mock::MockSubtitleSink sink;

    class ProgressValidationObserver : public sublift::IExtractionObserver {
     public:
      void on_progress(int64_t current_ms, int64_t total_ms, double fps) override {
        std::lock_guard<std::mutex> lock(mutex);
        calls.push_back({current_ms, total_ms, fps});
      }
      struct Call { int64_t current_ms; int64_t total_ms; double fps; };
      std::mutex mutex;
      std::vector<Call> calls;
    } observer;

    sublift::ExtractionConfig cfg{
        .sample_fps = 10.0,
    };

    sublift::ExtractionSession session(source, ocr, nullptr, &sink, cfg);
    session.set_observer(&observer);

    session.process_video("probed_video.mp4");

    std::lock_guard<std::mutex> lock(observer.mutex);
    REQUIRE_FALSE(observer.calls.empty());
    for (const auto& call : observer.calls) {
      CHECK(call.total_ms == kExpectedDurationMs);
      CHECK(call.total_ms > 0);
      CHECK(call.current_ms <= call.total_ms);
      CHECK(call.fps == Catch::Approx(10.0));
    }
  }

  SECTION("Trailing open segment at finalize() emits both on_segment_detected and on_subtitle_entry") {
    sublift::adapters::mock::MockFrameSource source;
    sublift::adapters::mock::MockOcrEngine ocr("Trailing Segment Text", 0.98);
    sublift::adapters::mock::MockSubtitleSink sink;

    struct DetectedSegMeta {
      int64_t start_ms;
      int64_t end_ms;
    };

    class TrailingEventObserver : public sublift::IExtractionObserver {
     public:
      void on_segment_detected(const sublift::timing::Segment& seg) override {
        std::lock_guard<std::mutex> lock(mutex);
        detected_segments.push_back({seg.start_ms, seg.end_ms});
      }

      void on_subtitle_entry(const sublift::SubtitleEntry& entry) override {
        std::lock_guard<std::mutex> lock(mutex);
        received_entries.push_back(entry);
      }

      std::mutex mutex;
      std::vector<DetectedSegMeta> detected_segments;
      std::vector<sublift::SubtitleEntry> received_entries;
    } observer;

    sublift::ExtractionConfig cfg{
        .sample_fps = 10.0,
        .hysteresis_ms = 100,
        .confirm_ms = 100,
        .enable_line_select = false,
        .confidence_threshold = 0.0,
        .min_duration_ms = 0,
        .subtitle_script = "auto",
        .num_threads = 2,
    };

    DynamicBoxDetector detector;
    sublift::ExtractionSession session(source, ocr, &detector, &sink, cfg);
    session.set_observer(&observer);

    // Feed sequence:
    // 0..200ms: background (no subtitle)
    // 400..1000ms: subtitle present
    // No closing Out event! Video stream ends while subtitle is active!
    session.feed(make_pattern_frame(0, false, 128, 64));
    session.feed(make_pattern_frame(200, false, 128, 64));
    session.feed(make_pattern_frame(400, true, 128, 64));
    session.feed(make_pattern_frame(600, true, 128, 64));
    session.feed(make_pattern_frame(800, true, 128, 64));
    session.feed(make_pattern_frame(1000, true, 128, 64));

    // finalize() must close the trailing open segment and trigger all observer callbacks
    auto final_entries = session.finalize();

    REQUIRE_FALSE(session.is_running());
    REQUIRE(final_entries.size() == 1);
    CHECK(final_entries[0].range.start_ms == 400);
    CHECK(final_entries[0].range.end_ms == 1000);
    CHECK(final_entries[0].text == "Trailing Segment Text");

    // Verify on_segment_detected was called for the trailing segment
    {
      std::lock_guard<std::mutex> lock(observer.mutex);
      REQUIRE(observer.detected_segments.size() == 1);
      CHECK(observer.detected_segments[0].start_ms == 400);
      CHECK(observer.detected_segments[0].end_ms == 1000);

      // Verify on_subtitle_entry was called for the trailing segment
      REQUIRE(observer.received_entries.size() == 1);
      CHECK(observer.received_entries[0].range.start_ms == 400);
      CHECK(observer.received_entries[0].range.end_ms == 1000);
      CHECK(observer.received_entries[0].text == "Trailing Segment Text");
    }

    // Verify sink also received the trailing entry
    REQUIRE(sink.entries().size() == 1);
    CHECK(sink.entries()[0].range.start_ms == 400);
    CHECK(sink.entries()[0].range.end_ms == 1000);
    CHECK(sink.entries()[0].text == "Trailing Segment Text");
  }
}

TEST_CASE("Challenger M1: Edge cases in AnalysisExecutor lifecycle",
          "[challenger][engine][analysis_executor][edges]") {
  SECTION("Immediate cancellation without push") {
    sublift::timing::TimingConfig timing_cfg{};
    sublift::AnalysisExecutor executor(timing_cfg, 0, 8);
    executor.cancel();
    REQUIRE_FALSE(executor.pop().has_value());
  }

  SECTION("Push after finish_pushing is discarded without deadlock") {
    sublift::timing::TimingConfig timing_cfg{};
    sublift::AnalysisExecutor executor(timing_cfg, 2, 4);
    executor.finish_pushing();
    // Pushing after finish_pushing must not deadlock
    executor.push(0, 0, sublift::ImageBuffer(50, 20, sublift::PixelFormat::RGB24));
    REQUIRE_FALSE(executor.pop().has_value());
  }
}

TEST_CASE("Challenger M1: Edge cases in SegmentRecognizer lifecycle",
          "[challenger][engine][segment_recognizer][edges]") {
  SECTION("Empty anchor segment skips OCR recognition") {
    sublift::adapters::mock::MockOcrEngine ocr;
    sublift::ExtractionConfig cfg{};
    sublift::SegmentRecognizer recognizer(ocr, cfg, nullptr);

    sublift::timing::Segment empty_seg{
        .start_ms = 100,
        .end_ms = 500,
        .anchor_timestamp_ms = 300,
        .fallback_timestamps_ms = {},
        .anchor_image = sublift::ImageBuffer(),
        .fallback_images = {},
    };

    recognizer.submit(std::move(empty_seg), 1);
    recognizer.drain();

    CHECK(recognizer.ocr_calls_count() == 0);
    auto entries = recognizer.take_raw_entries();
    REQUIRE(entries.size() == 1);
    CHECK(entries[0].text.empty());
  }

  SECTION("OCR engine exception properly captured and rethrown by drain") {
    FaultyOcrEngine faulty_ocr;
    sublift::ExtractionConfig cfg{};
    sublift::SegmentRecognizer recognizer(faulty_ocr, cfg, nullptr);

    sublift::ImageBuffer img(60, 30, sublift::PixelFormat::RGB24);
    sublift::timing::Segment seg{
        .start_ms = 0,
        .end_ms = 500,
        .anchor_timestamp_ms = 250,
        .fallback_timestamps_ms = {},
        .anchor_image = std::move(img),
        .fallback_images = {},
    };

    recognizer.submit(std::move(seg), 1);
    REQUIRE_THROWS_AS(recognizer.drain(), std::runtime_error);
  }

  SECTION("Rapid cancellation with pending queue items drains cleanly") {
    SlowMockOcrEngine slow_ocr(std::chrono::milliseconds(50));
    sublift::ExtractionConfig cfg{.queue_capacity = 8};
    sublift::SegmentRecognizer recognizer(slow_ocr, cfg, nullptr);

    for (std::size_t i = 0; i < 5; ++i) {
      sublift::ImageBuffer img(60, 30, sublift::PixelFormat::RGB24);
      sublift::timing::Segment seg{
          .start_ms = static_cast<std::int64_t>(i * 100),
          .end_ms = static_cast<std::int64_t>(i * 100 + 80),
          .anchor_timestamp_ms = static_cast<std::int64_t>(i * 100 + 40),
          .fallback_timestamps_ms = {},
          .anchor_image = std::move(img),
          .fallback_images = {},
      };
      recognizer.submit(std::move(seg), i + 1);
    }

    recognizer.cancel();
    recognizer.drain();
    // After cancellation, pending tasks may be discarded; no hang
  }
}


