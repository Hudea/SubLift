#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <sublift/adapters/mock/mock_frame_source.hpp>
#include <sublift/adapters/mock/mock_ocr.hpp>
#include <sublift/adapters/mock/mock_subtitle_sink.hpp>
#include <sublift/core/box.hpp>
#include <sublift/core/color.hpp>
#include <sublift/core/frame.hpp>
#include <sublift/core/image_buffer.hpp>
#include <sublift/core/models.hpp>
#include <sublift/core/subtitle.hpp>
#include <sublift/engine/config.hpp>
#include <sublift/engine/observer.hpp>
#include <sublift/engine/session.hpp>
#include <sublift/ports/detector.hpp>
#include <sublift/ports/ocr_engine.hpp>
#include <sublift/ports/subtitle_sink.hpp>
#include <sublift/text/srt.hpp>

namespace {

class RecordingSubtitleSink final : public sublift::ISubtitleSink {
 public:
  void emit(const sublift::SubtitleEntry& entry) override {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.push_back(entry);
  }

  void flush() override {
    std::lock_guard<std::mutex> lock(mutex_);
    flushed_ = true;
  }

  void close() override {
    std::lock_guard<std::mutex> lock(mutex_);
    flushed_ = true;
    closed_ = true;
  }

  [[nodiscard]] std::string to_srt_string() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return sublift::text::format_srt(entries_);
  }

  [[nodiscard]] bool is_flushed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return flushed_;
  }

  [[nodiscard]] bool is_closed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return closed_;
  }

 private:
  mutable std::mutex mutex_;
  std::vector<sublift::SubtitleEntry> entries_;
  bool flushed_{false};
  bool closed_{false};
};

class IntegrationObserver final : public sublift::IExtractionObserver {
 public:
  void on_progress(std::int64_t, std::int64_t, double) override {
    progress_count_.fetch_add(1, std::memory_order_relaxed);
  }

  void on_segment_detected(const sublift::timing::Segment&) override {}
  void on_subtitle_entry(const sublift::SubtitleEntry&) override {}
  void on_error(const sublift::ExtractionError&) override {}

  void on_complete(const sublift::ExtractionMetrics& metrics) override {
    std::lock_guard<std::mutex> lock(mutex_);
    completed_ = true;
    final_metrics_ = metrics;
  }

  [[nodiscard]] std::size_t progress_count() const noexcept {
    return progress_count_.load(std::memory_order_relaxed);
  }

  [[nodiscard]] bool is_completed() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return completed_;
  }

  [[nodiscard]] sublift::ExtractionMetrics final_metrics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return final_metrics_;
  }

 private:
  mutable std::mutex mutex_;
  std::atomic<std::size_t> progress_count_{0};
  bool completed_{false};
  sublift::ExtractionMetrics final_metrics_{};
};

sublift::Frame create_synthetic_frame(
    std::int64_t timestamp_ms,
    std::int32_t width = 320,
    std::int32_t height = 180,
    bool draw_subtitle = false,
    std::uint8_t bg_val = 30,
    std::uint8_t sub_val = 240) {
  if (width <= 0 || height <= 0) {
    return sublift::Frame{
        .timestamp_ms = timestamp_ms,
        .image = sublift::ImageBuffer(),
    };
  }

  sublift::ImageBuffer img(width, height, sublift::PixelFormat::RGB24);
  for (std::int32_t y = 0; y < height; ++y) {
    std::uint8_t* row = img.row(y);
    for (std::int32_t x = 0; x < width; ++x) {
      row[x * 3 + 0] = bg_val;
      row[x * 3 + 1] = bg_val;
      row[x * 3 + 2] = bg_val;
    }
  }

  if (draw_subtitle && width >= 32 && height >= 16) {
    const std::int32_t x_start = width / 4;
    const std::int32_t x_end = (width * 3) / 4;
    const std::int32_t y_start = (height * 3) / 4;
    const std::int32_t y_end = (height * 9) / 10;

    for (std::int32_t y = y_start; y < y_end; ++y) {
      std::uint8_t* row = img.row(y);
      for (std::int32_t x = x_start; x < x_end; ++x) {
        row[x * 3 + 0] = sub_val;
        row[x * 3 + 1] = sub_val;
        row[x * 3 + 2] = sub_val;
      }
    }
  }

  return sublift::Frame{
      .timestamp_ms = timestamp_ms,
      .image = std::move(img),
  };
}

class BottomRatioDetector final : public sublift::IDetector {
 public:
  explicit BottomRatioDetector(double ratio = 0.3) : ratio_(ratio) {}

  std::optional<sublift::Region> detect(const sublift::Frame& frame) override {
    const std::int32_t w = frame.image.width();
    const std::int32_t h = frame.image.height();
    if (w <= 0 || h <= 0) return std::nullopt;
    std::int32_t crop_h = static_cast<std::int32_t>(h * ratio_);
    crop_h = std::clamp(crop_h, 1, h);
    return sublift::Region{sublift::FrameLocalBox{0, h - crop_h, w, crop_h}};
  }

 private:
  double ratio_{0.3};
};

class FixedBoxDetector final : public sublift::IDetector {
 public:
  explicit FixedBoxDetector(sublift::FrameLocalBox box) : box_(box) {}

  std::optional<sublift::Region> detect(const sublift::Frame&) override {
    return sublift::Region{box_};
  }

 private:
  sublift::FrameLocalBox box_;
};

class ScriptedOcrEngine final : public sublift::IOcrEngine {
 public:
  struct Response {
    std::string text;
    double confidence{0.95};
    std::vector<sublift::OcrLine> lines{};
  };

  explicit ScriptedOcrEngine(
      std::vector<Response> responses = {},
      std::chrono::microseconds artificial_delay = std::chrono::microseconds(0))
      : responses_(std::move(responses)), delay_(artificial_delay) {}

  sublift::OcrResult recognize(const sublift::ImageView&) override {
    if (delay_.count() > 0) {
      std::this_thread::sleep_for(delay_);
    }

    std::lock_guard<std::mutex> lock(mutex_);
    if (responses_.empty()) {
      return sublift::OcrResult{
          .text = "Default Scripted Subtitle",
          .confidence = 0.95,
          .lines = {},
      };
    }

    const std::size_t idx =
        current_idx_ < responses_.size() ? current_idx_++ : responses_.size() - 1;
    const auto& resp = responses_[idx];
    if (!resp.lines.empty()) {
      return sublift::OcrResult::from_lines(resp.lines);
    }
    return sublift::OcrResult{
        .text = resp.text,
        .confidence = resp.confidence,
        .lines = {},
    };
  }

  [[nodiscard]] std::string engine_name() const override {
    return "ScriptedOcrEngine";
  }

 private:
  mutable std::mutex mutex_;
  std::vector<Response> responses_;
  std::chrono::microseconds delay_{0};
  std::size_t current_idx_{0};
};

}  // namespace

TEST_CASE("Session integration: standard synthetic pipeline assembly", "[integration][engine]") {
  sublift::adapters::mock::MockFrameSource frame_source;
  ScriptedOcrEngine ocr({
      {.text = "First Subtitle Entry", .confidence = 0.96},
      {.text = "Second Subtitle Entry", .confidence = 0.98},
  });
  BottomRatioDetector detector(0.3);
  RecordingSubtitleSink sink;

  sublift::ExtractionConfig config{
      .sample_fps = 5.0,
      .region_bottom_ratio = 0.3,
      .hysteresis_ms = 100,
      .confirm_ms = 100,
      .min_duration_ms = 300,
      .num_threads = 1,
  };

  sublift::ExtractionSession session(frame_source, ocr, &detector, &sink, config);
  IntegrationObserver observer;
  session.set_observer(&observer);

  REQUIRE_FALSE(session.is_running());
  REQUIRE_FALSE(session.is_cancelled());

  session.feed(create_synthetic_frame(0, 320, 180, false));
  for (int t = 200; t <= 800; t += 200) {
    session.feed(create_synthetic_frame(t, 320, 180, true));
  }
  for (int t = 1000; t <= 1400; t += 200) {
    session.feed(create_synthetic_frame(t, 320, 180, false));
  }
  for (int t = 1600; t <= 2200; t += 200) {
    session.feed(create_synthetic_frame(t, 320, 180, true));
  }
  session.feed(create_synthetic_frame(2400, 320, 180, false));

  auto entries = session.finalize();

  REQUIRE_FALSE(session.is_running());
  REQUIRE(observer.is_completed());
  REQUIRE(sink.is_flushed());
  REQUIRE(sink.is_closed());

  const auto metrics = session.metrics();
  REQUIRE(metrics.total_frames_processed == 13);
  REQUIRE(metrics.total_segments_detected >= 1);
  REQUIRE(entries.size() >= 1);

  const std::string srt = sink.to_srt_string();
  REQUIRE_FALSE(srt.empty());
  REQUIRE(srt.find("-->") != std::string::npos);
}

TEST_CASE("Session integration: multi-line assembly and SRT formatting", "[integration][ocr][srt]") {
  const std::vector<sublift::OcrLine> lines = {
      sublift::OcrLine{
          .text = "第一行中文字幕",
          .confidence = 0.98,
          .box = sublift::OcrCropBox{10, 10, 100, 20},
      },
      sublift::OcrLine{
          .text = "Second line english subtitle",
          .confidence = 0.96,
          .box = sublift::OcrCropBox{10, 35, 120, 20},
      },
  };

  const auto ocr_res = sublift::OcrResult::from_lines(lines);
  REQUIRE(ocr_res.lines.size() == 2);
  REQUIRE(ocr_res.text == "第一行中文字幕\nSecond line english subtitle");
  REQUIRE(ocr_res.confidence == Catch::Approx(0.97));

  CHECK(sublift::text::format_timestamp_srt(0) == "00:00:00,000");
  CHECK(sublift::text::format_timestamp_srt(3661005) == "01:01:01,005");
  CHECK(sublift::text::format_timestamp_srt(-500) == "00:00:00,000");

  const std::vector<sublift::SubtitleEntry> entries = {
      sublift::SubtitleEntry{
          .id = 1,
          .range = sublift::TimeRange{.start_ms = 1000, .end_ms = 3500},
          .text = ocr_res.text,
          .confidence = ocr_res.confidence,
      },
  };
  const std::string srt = sublift::text::format_srt(entries);
  CHECK(srt == "1\n00:00:01,000 --> 00:00:03,500\n第一行中文字幕\nSecond line english subtitle\n\n");
}

TEST_CASE("Session integration: observer lifecycle and metrics tracking", "[integration][observer]") {
  sublift::adapters::mock::MockFrameSource source;
  ScriptedOcrEngine ocr({{.text = "Observer Verification", .confidence = 0.95}});
  FixedBoxDetector detector(sublift::FrameLocalBox{0, 126, 320, 54});
  sublift::adapters::mock::MockSubtitleSink sink;

  sublift::ExtractionConfig cfg{
      .sample_fps = 5.0,
      .hysteresis_ms = 100,
      .confirm_ms = 100,
      .min_duration_ms = 200,
  };

  sublift::ExtractionSession session(source, ocr, &detector, &sink, cfg);
  IntegrationObserver observer;
  session.set_observer(&observer);

  session.feed(create_synthetic_frame(0, 320, 180, false));
  session.feed(create_synthetic_frame(200, 320, 180, true));
  session.feed(create_synthetic_frame(400, 320, 180, true));
  session.feed(create_synthetic_frame(600, 320, 180, false));

  REQUIRE(observer.progress_count() >= 3);
  session.finalize();

  REQUIRE(observer.is_completed());
  const auto m = observer.final_metrics();
  CHECK(m.total_frames_processed == 4);
}

TEST_CASE("Session integration: empty input and zero-frame boundaries", "[integration][boundary]") {
  sublift::adapters::mock::MockFrameSource source;
  ScriptedOcrEngine ocr;
  RecordingSubtitleSink sink;

  sublift::ExtractionSession session(source, ocr, nullptr, &sink, {});
  IntegrationObserver observer;
  session.set_observer(&observer);

  SECTION("Finalize called with zero frames fed") {
    auto entries = session.finalize();
    CHECK(entries.empty());
    CHECK(observer.is_completed());
    CHECK(sink.is_closed());
    CHECK(session.metrics().total_frames_processed == 0);
  }

  SECTION("Feeding empty uninitialized buffers") {
    session.feed(sublift::Frame{.timestamp_ms = 0, .image = sublift::ImageBuffer()});
    session.feed(sublift::Frame{.timestamp_ms = 100,
                                .image = sublift::ImageBuffer(0, 0, sublift::PixelFormat::RGB24)});
    auto entries = session.finalize();
    CHECK(entries.empty());
    CHECK(session.metrics().total_frames_processed == 0);
  }
}

TEST_CASE("Session integration: extreme resolution geometries and aspect ratios",
          "[integration][boundary][geometry]") {
  sublift::adapters::mock::MockFrameSource source;
  ScriptedOcrEngine ocr({{.text = "Geometry Edge", .confidence = 0.9}});
  BottomRatioDetector detector(0.3);
  sublift::adapters::mock::MockSubtitleSink sink;

  sublift::ExtractionSession session(source, ocr, &detector, &sink, {});

  SECTION("Minimal 1x1 frame") {
    session.feed(create_synthetic_frame(0, 1, 1, false));
    session.feed(create_synthetic_frame(200, 1, 1, true));
    session.feed(create_synthetic_frame(400, 1, 1, false));
    static_cast<void>(session.finalize());
    REQUIRE_FALSE(session.is_running());
  }

  SECTION("4K UHD 3840x2160") {
    session.feed(create_synthetic_frame(0, 3840, 2160, false));
    session.feed(create_synthetic_frame(200, 3840, 2160, true));
    session.feed(create_synthetic_frame(400, 3840, 2160, false));
    static_cast<void>(session.finalize());
    REQUIRE_FALSE(session.is_running());
  }

  SECTION("Wide banner 2048x2") {
    session.feed(create_synthetic_frame(0, 2048, 2, false));
    session.feed(create_synthetic_frame(200, 2048, 2, true));
    static_cast<void>(session.finalize());
    REQUIRE_FALSE(session.is_running());
  }

  SECTION("Tall pillar 2x2048") {
    session.feed(create_synthetic_frame(0, 2, 2048, false));
    session.feed(create_synthetic_frame(200, 2, 2048, true));
    static_cast<void>(session.finalize());
    REQUIRE_FALSE(session.is_running());
  }
}

TEST_CASE("Session integration: zero-ms anchor and large timestamp jump",
          "[integration][boundary][timestamps]") {
  sublift::adapters::mock::MockFrameSource source;
  ScriptedOcrEngine ocr({{.text = "Anchor at 0ms", .confidence = 0.95}});
  BottomRatioDetector detector(0.3);
  sublift::adapters::mock::MockSubtitleSink sink;

  sublift::ExtractionConfig cfg{
      .sample_fps = 5.0,
      .hysteresis_ms = 50,
      .confirm_ms = 50,
      .min_duration_ms = 100,
  };

  sublift::ExtractionSession session(source, ocr, &detector, &sink, cfg);

  session.feed(create_synthetic_frame(0, 320, 180, true));
  session.feed(create_synthetic_frame(100, 320, 180, true));
  session.feed(create_synthetic_frame(200, 320, 180, false));

  session.feed(create_synthetic_frame(3600000, 320, 180, true));
  session.feed(create_synthetic_frame(3600200, 320, 180, true));
  session.feed(create_synthetic_frame(3600400, 320, 180, false));

  static_cast<void>(session.finalize());
  REQUIRE_FALSE(session.is_running());
  CHECK(session.metrics().total_frames_processed == 6);
}

TEST_CASE("Session integration: cancellation idempotency and concurrent callers",
          "[integration][boundary][cancel]") {
  SECTION("Cancel before feeding frames") {
    sublift::adapters::mock::MockFrameSource source;
    ScriptedOcrEngine ocr;
    sublift::adapters::mock::MockSubtitleSink sink;

    sublift::ExtractionSession session(source, ocr, nullptr, &sink, {});
    session.cancel();
    REQUIRE(session.is_cancelled());

    session.feed(create_synthetic_frame(0, 100, 50, true));
    session.feed(create_synthetic_frame(100, 100, 50, true));

    auto entries = session.finalize();
    CHECK(entries.empty());
    CHECK_FALSE(session.is_running());
  }

  SECTION("Cancel called concurrently from 8 threads") {
    sublift::adapters::mock::MockFrameSource source;
    ScriptedOcrEngine ocr;
    sublift::adapters::mock::MockSubtitleSink sink;

    sublift::ExtractionSession session(source, ocr, nullptr, &sink, {});

    std::vector<std::thread> cancellers;
    cancellers.reserve(8);
    for (int i = 0; i < 8; ++i) {
      cancellers.emplace_back([&session]() {
        for (int k = 0; k < 25; ++k) {
          session.cancel();
        }
      });
    }
    for (auto& t : cancellers) {
      t.join();
    }

    REQUIRE(session.is_cancelled());
    auto entries = session.finalize();
    CHECK(entries.empty());
  }
}

TEST_CASE("Session integration: backpressure queue capacity = 1",
          "[integration][boundary][backpressure]") {
  sublift::adapters::mock::MockFrameSource source;
  ScriptedOcrEngine ocr({{.text = "Backpressure", .confidence = 0.9}},
                        std::chrono::microseconds(500));
  sublift::adapters::mock::MockSubtitleSink sink;

  sublift::ExtractionConfig cfg{
      .sample_fps = 10.0,
      .num_threads = 4,
      .queue_capacity = 1,
  };

  sublift::ExtractionSession session(source, ocr, nullptr, &sink, cfg);

  constexpr int kTotalFrames = 60;
  for (int i = 0; i < kTotalFrames; ++i) {
    const bool has_sub = (i >= 15 && i <= 40);
    session.feed(create_synthetic_frame(i * 100, 160, 90, has_sub));
  }

  static_cast<void>(session.finalize());
  REQUIRE_FALSE(session.is_running());
  CHECK(session.metrics().total_frames_processed == kTotalFrames);
}

TEST_CASE("Session integration: mid-stream cancellation terminates cleanly",
          "[integration][streaming-cancel]") {
  sublift::adapters::mock::MockFrameSource source;
  ScriptedOcrEngine ocr({{.text = "Streaming", .confidence = 0.95}},
                        std::chrono::milliseconds(1));
  sublift::adapters::mock::MockSubtitleSink sink;

  sublift::ExtractionConfig cfg{
      .sample_fps = 10.0,
      .num_threads = 4,
      .queue_capacity = 4,
  };

  sublift::ExtractionSession session(source, ocr, nullptr, &sink, cfg);

  std::promise<void> started_promise;
  std::future<void> started_future = started_promise.get_future();
  bool signaled = false;

  auto producer = std::async(std::launch::async, [&]() {
    for (int i = 0; i < 300; ++i) {
      if (session.is_cancelled()) break;
      session.feed(create_synthetic_frame(i * 100, 160, 90, (i % 2 == 0)));
      if (!signaled && i >= 3) {
        signaled = true;
        started_promise.set_value();
      }
    }
  });

  REQUIRE(started_future.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
  session.cancel();

  REQUIRE(producer.wait_for(std::chrono::seconds(10)) == std::future_status::ready);

  static_cast<void>(session.finalize());
  REQUIRE(session.is_cancelled());
  REQUIRE_FALSE(session.is_running());
}

TEST_CASE("Session integration: bottom-crop detector consensus and temporal deduplication",
          "[integration][consensus][dedupe]") {
  sublift::adapters::mock::MockFrameSource source;
  const std::vector<ScriptedOcrEngine::Response> responses = {
      {.text = "动物城大冒险", .confidence = 0.95},
      {.text = "动物城大冒险", .confidence = 0.96},
      {.text = "动物城大冒险", .confidence = 0.97},
  };
  ScriptedOcrEngine ocr(responses);
  BottomRatioDetector detector(0.3);
  RecordingSubtitleSink sink;

  sublift::ExtractionConfig cfg{
      .sample_fps = 5.0,
      .region_bottom_ratio = 0.3,
      .hysteresis_ms = 100,
      .confirm_ms = 100,
      .ocr_consensus_frames = 3,
      .min_duration_ms = 200,
      .merge_gap_ms = 800,
      .num_threads = 1,
  };

  sublift::ExtractionSession session(source, ocr, &detector, &sink, cfg);

  for (int t = 0; t <= 600; t += 200) {
    session.feed(create_synthetic_frame(t, 320, 180, true));
  }
  session.feed(create_synthetic_frame(800, 320, 180, false));
  session.feed(create_synthetic_frame(1000, 320, 180, false));
  for (int t = 1200; t <= 1800; t += 200) {
    session.feed(create_synthetic_frame(t, 320, 180, true));
  }
  session.feed(create_synthetic_frame(2000, 320, 180, false));

  auto entries = session.finalize();
  REQUIRE_FALSE(entries.empty());
  CHECK(entries.size() == 1);
  CHECK(entries[0].text == "动物城大冒险");
}

TEST_CASE("Session integration: observer exception resiliency",
          "[integration][exceptions]") {
  class FaultyIntegrationObserver final : public sublift::IExtractionObserver {
   public:
    void on_progress(std::int64_t, std::int64_t, double) override {
      if (should_throw) {
        throw std::runtime_error("FaultyObserver: intentional exception in on_progress");
      }
    }
    bool should_throw{false};
  };

  sublift::adapters::mock::MockFrameSource source;
  ScriptedOcrEngine ocr;
  sublift::adapters::mock::MockSubtitleSink sink;

  sublift::ExtractionConfig cfg{
      .sample_fps = 5.0,
      .num_threads = 2,
  };

  sublift::ExtractionSession session(source, ocr, nullptr, &sink, cfg);
  FaultyIntegrationObserver observer;
  observer.should_throw = true;
  session.set_observer(&observer);

  try {
    session.feed(create_synthetic_frame(0, 160, 90, false));
    session.feed(create_synthetic_frame(200, 160, 90, true));
  } catch (const std::runtime_error& e) {
    CHECK(std::string(e.what()).find("FaultyObserver") != std::string::npos);
  }

  session.cancel();
  static_cast<void>(session.finalize());
  REQUIRE_FALSE(session.is_running());
}
