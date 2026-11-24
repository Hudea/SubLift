#include <catch2/catch_test_macros.hpp>
#include <sublift/adapters/mock/mock_frame_source.hpp>
#include <sublift/adapters/mock/mock_ocr.hpp>
#include <sublift/adapters/mock/mock_subtitle_sink.hpp>
#include <sublift/engine/analysis_executor.hpp>
#include <sublift/engine/config.hpp>
#include <sublift/engine/observer.hpp>
#include <sublift/engine/segment_recognizer.hpp>
#include <sublift/engine/session.hpp>
#include <sublift/text/srt.hpp>
#include <sublift/timing/timing_config.hpp>

namespace {

class EngineTestObserver : public sublift::IExtractionObserver {
 public:
  void on_progress(std::int64_t, std::int64_t, double) override { progress_calls++; }
  void on_segment_detected(const sublift::timing::Segment&) override { segment_calls++; }
  void on_subtitle_entry(const sublift::SubtitleEntry&) override { entry_calls++; }
  void on_complete(const sublift::ExtractionMetrics& metrics) override {
    completed = true;
    last_metrics = metrics;
  }

  int progress_calls{0};
  int segment_calls{0};
  int entry_calls{0};
  bool completed{false};
  sublift::ExtractionMetrics last_metrics{};
};

}  // namespace

TEST_CASE("Engine orchestrator session lifecycle", "[engine]") {
  sublift::adapters::mock::MockFrameSource source;
  sublift::adapters::mock::MockOcrEngine ocr("Detected subtitle", 0.95);
  sublift::adapters::mock::MockSubtitleSink sink;

  sublift::ExtractionConfig cfg{
      .sample_fps = 5.0,
      .hysteresis_ms = 100,
      .confirm_ms = 100,
      .min_duration_ms = 300,
  };

  sublift::ExtractionSession session(source, ocr, nullptr, &sink, cfg);
  EngineTestObserver observer;
  session.set_observer(&observer);

  REQUIRE_FALSE(session.is_running());
  REQUIRE_FALSE(session.is_cancelled());

  session.feed(sublift::Frame{
      .timestamp_ms = 0,
      .image = sublift::ImageBuffer(100, 40, sublift::PixelFormat::RGB24),
  });

  sublift::ImageBuffer sub_buf(100, 40, sublift::PixelFormat::RGB24);
  for (int y = 20; y < 35; ++y) {
    std::uint8_t* row = sub_buf.row(y);
    for (int x = 20; x < 80; ++x) {
      row[x * 3 + 0] = 255;
      row[x * 3 + 1] = 255;
      row[x * 3 + 2] = 255;
    }
  }

  session.feed(sublift::Frame{.timestamp_ms = 200, .image = sub_buf.clone()});
  session.feed(sublift::Frame{.timestamp_ms = 400, .image = sub_buf.clone()});
  session.feed(sublift::Frame{.timestamp_ms = 600, .image = sub_buf.clone()});

  session.feed(sublift::Frame{
      .timestamp_ms = 800,
      .image = sublift::ImageBuffer(100, 40, sublift::PixelFormat::RGB24),
  });
  session.feed(sublift::Frame{
      .timestamp_ms = 1000,
      .image = sublift::ImageBuffer(100, 40, sublift::PixelFormat::RGB24),
  });

  REQUIRE(observer.progress_calls >= 5);

  static_cast<void>(session.finalize());
  REQUIRE_FALSE(session.is_running());
  REQUIRE(observer.completed);
  REQUIRE(sink.is_flushed());
  REQUIRE(session.metrics().total_frames_processed == 6);
}

TEST_CASE("AnalysisExecutor parallel reorder and hardware concurrency fallback",
          "[engine][analysis_executor]") {
  sublift::timing::TimingConfig timing_cfg{};
  sublift::AnalysisExecutor executor(timing_cfg, /*num_threads=*/0, /*queue_capacity=*/16);

  for (std::uint64_t i = 0; i < 10; ++i) {
    sublift::ImageBuffer img(100, 40, sublift::PixelFormat::RGB24);
    executor.push(i, static_cast<std::int64_t>(i * 100), std::move(img));
  }
  executor.finish_pushing();

  std::uint64_t expected_idx = 0;
  while (auto frame_opt = executor.pop()) {
    REQUIRE(frame_opt->frame_index == expected_idx);
    REQUIRE(frame_opt->timestamp_ms == static_cast<std::int64_t>(expected_idx * 100));
    ++expected_idx;
  }
  REQUIRE(expected_idx == 10);
}

TEST_CASE("SegmentRecognizer async queue, observer notification and memory cleanup",
          "[engine][segment_recognizer]") {
  sublift::adapters::mock::MockOcrEngine ocr("Segment OCR Output", 0.95);
  sublift::ExtractionConfig cfg{};
  EngineTestObserver observer;

  sublift::SegmentRecognizer recognizer(ocr, cfg, &observer);

  sublift::ImageBuffer anchor(100, 40, sublift::PixelFormat::RGB24);
  sublift::ImageBuffer fallback(100, 40, sublift::PixelFormat::RGB24);

  sublift::timing::Segment seg{
      .start_ms = 1000,
      .end_ms = 2000,
      .anchor_timestamp_ms = 1200,
      .fallback_timestamps_ms = {1400},
      .anchor_image = std::move(anchor),
      .fallback_images = {},
  };
  seg.fallback_images.push_back(std::move(fallback));

  recognizer.submit(std::move(seg), 1);
  recognizer.drain();

  REQUIRE(observer.entry_calls == 1);
  REQUIRE(recognizer.ocr_calls_count() == 1);

  auto raw_entries = recognizer.take_raw_entries();
  REQUIRE(raw_entries.size() == 1);
  REQUIRE(raw_entries[0].id == 1);
  REQUIRE(raw_entries[0].range.start_ms == 1000);
  REQUIRE(raw_entries[0].range.end_ms == 2000);
  REQUIRE(raw_entries[0].text == "Segment OCR Output");
}

namespace {

class FailIfCalledOcrEngine final : public sublift::IOcrEngine {
 public:
  sublift::OcrResult recognize(const sublift::ImageView&) override {
    called = true;
    return {};
  }
  [[nodiscard]] std::string engine_name() const override { return "fail_if_called"; }
  bool called{false};
};

sublift::ImageBuffer make_timing_band_frame(int width, int height, bool has_subtitle) {
  sublift::ImageBuffer img(width, height, sublift::PixelFormat::RGB24);
  if (has_subtitle) {
    for (int y = 10; y < 30; ++y) {
      std::uint8_t* row = img.row(y);
      for (int x = 40; x < width - 40; ++x) {
        row[x * 3 + 0] = 255;
        row[x * 3 + 1] = 255;
        row[x * 3 + 2] = 255;
      }
    }
  }
  return img;
}

}  // namespace

TEST_CASE("Pure timing session skips OCR and preserves short and adjacent segments", "[engine][pure_timing]") {
  sublift::adapters::mock::MockFrameSource source;
  // 200ms ~ 400ms (200ms short cue) and 1200ms ~ 1600ms (400ms cue), separated by blank frames
  for (int t = 0; t <= 2400; t += 200) {
    const bool has_sub = (t >= 200 && t <= 400) || (t >= 1200 && t <= 1600);
    source.add_frame(sublift::Frame{
        .timestamp_ms = t,
        .image = make_timing_band_frame(320, 40, has_sub),
    });
  }

  FailIfCalledOcrEngine ocr;
  sublift::ExtractionConfig config;
  config.sample_fps = 5.0;
  config.hysteresis_ms = 100;
  config.confirm_ms = 100;
  config.region_bottom_ratio = 0.0;
  config.pure_timing = true;
  config.min_duration_ms = 0;
  config.drop_empty_text = false;
  config.num_threads = 1;

  sublift::ExtractionSession session(source, ocr, nullptr, nullptr, config);
  auto entries = session.process_video("/mock/pure_timing.mp4");

  REQUIRE_FALSE(ocr.called);
  REQUIRE(session.metrics().total_ocr_calls == 0);
  REQUIRE(entries.size() == 2);
  for (auto& e : entries) {
    e.text = "TIMING_ONLY";
  }
  auto [srt, cue_count] = sublift::text::format_srt_counted(entries);
  REQUIRE(cue_count == 2);
  REQUIRE(srt.find("TIMING_ONLY") != std::string::npos);
}
