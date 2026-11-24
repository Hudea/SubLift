#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <sublift/adapters/mock/mock_frame_source.hpp>
#include <sublift/adapters/mock/mock_ocr.hpp>
#include <sublift/adapters/mock/mock_subtitle_sink.hpp>

TEST_CASE("Mock adapters contract tests", "[adapters][mock]") {
  SECTION("MockFrameSource probe, plan and extract") {
    sublift::adapters::mock::MockFrameSource source;
    auto info = source.probe("sample.mp4");
    REQUIRE(info.width == 1920);
    REQUIRE(info.height == 1080);

    auto plan = source.plan(info, sublift::SourceBox{0, 720, 1920, 360});
    REQUIRE(plan.output_mode == sublift::FrameOutputMode::RoiRgb);
    REQUIRE(plan.output_crop.has_value());

    int frame_count = 0;
    source.extract("sample.mp4", 5.0, std::nullopt, [&](sublift::Frame f) {
      frame_count++;
      REQUIRE_FALSE(f.image.empty());
      return true;
    });
    REQUIRE(frame_count == 5);

    source.cancel();
    REQUIRE(source.is_cancelled());
  }

  SECTION("MockOcrEngine recognition and queuing") {
    sublift::adapters::mock::MockOcrEngine ocr("Default text", 0.90);
    REQUIRE(ocr.engine_name() == "MockOcrEngine");
    REQUIRE(ocr.call_count() == 0);

    sublift::ImageBuffer buf(50, 20, sublift::PixelFormat::RGB24);
    auto res1 = ocr.recognize(buf.view());
    REQUIRE(res1.text == "Default text");
    REQUIRE(res1.confidence == 0.90);
    REQUIRE(ocr.call_count() == 1);

    ocr.queue_result(sublift::OcrResult{
        .text = "Custom Queued",
        .confidence = 0.99,
        .lines = {},
    });

    auto res2 = ocr.recognize(buf.view());
    REQUIRE(res2.text == "Custom Queued");
    REQUIRE(res2.confidence == 0.99);
    REQUIRE(ocr.call_count() == 2);

    auto res3 = ocr.recognize(buf.view());
    REQUIRE(res3.text == "Default text");
    REQUIRE(ocr.call_count() == 3);
  }

  SECTION("MockOcrEngine sequence mode with padding and repeat") {
    std::vector<sublift::OcrResult> seq{
        sublift::OcrResult{.text = "Seq 1", .confidence = 0.80, .lines = {}},
        sublift::OcrResult{.text = "Seq 2", .confidence = 0.85, .lines = {}},
        sublift::OcrResult{.text = "Seq 3", .confidence = 0.90, .lines = {}},
    };
    sublift::adapters::mock::MockOcrEngine ocr(std::move(seq));
    REQUIRE(ocr.is_sequence_mode());
    REQUIRE(ocr.sequence_size() >= 11);

    sublift::ImageBuffer buf(50, 20, sublift::PixelFormat::RGB24);
    REQUIRE(ocr.call_count() == 0);

    auto r1 = ocr.recognize(buf.view());
    REQUIRE(r1.text == "Seq 1");
    REQUIRE(ocr.call_count() == 1);

    auto r2 = ocr.recognize(buf.view());
    REQUIRE(r2.text == "Seq 2");
    REQUIRE(ocr.call_count() == 2);

    auto r3 = ocr.recognize(buf.view());
    REQUIRE(r3.text == "Seq 3");
    REQUIRE(ocr.call_count() == 3);

    for (int i = 4; i <= 20; ++i) {
      auto ri = ocr.recognize(buf.view());
      REQUIRE(ri.text == "Seq 3");
      REQUIRE(ri.confidence == 0.90);
      REQUIRE(ocr.call_count() == static_cast<std::size_t>(i));
    }
  }

  SECTION("MockOcrEngine candidate lines for line selection scenarios") {
    std::vector<sublift::OcrLine> candidate_lines{
        sublift::OcrLine{
            .text = "Top Line Subtitle",
            .confidence = 0.92,
            .box = sublift::OcrCropBox{10, 5, 80, 15},
        },
        sublift::OcrLine{
            .text = "Bottom Line Subtitle",
            .confidence = 0.88,
            .box = sublift::OcrCropBox{10, 22, 80, 15},
        },
    };

    sublift::adapters::mock::MockOcrEngine ocr(std::span<const sublift::OcrLine>{candidate_lines});
    sublift::ImageBuffer buf(100, 40, sublift::PixelFormat::RGB24);

    auto res = ocr.recognize(buf.view());
    REQUIRE(res.lines.size() == 2);
    REQUIRE(res.lines[0].text == "Top Line Subtitle");
    REQUIRE(res.lines[0].box.y == 5);
    REQUIRE(res.lines[1].text == "Bottom Line Subtitle");
    REQUIRE(res.lines[1].box.y == 22);
    REQUIRE(res.text == "Top Line Subtitle\nBottom Line Subtitle");
    REQUIRE(ocr.call_count() == 1);
  }

  SECTION("MockOcrEngine multi-threaded concurrent recognize and call_count") {
    sublift::adapters::mock::MockOcrEngine ocr("Concurrent OCR", 0.95);
    constexpr int num_threads = 8;
    constexpr int calls_per_thread = 100;
    std::vector<std::thread> workers;
    workers.reserve(num_threads);

    std::atomic<bool> all_matched{true};
    for (int t = 0; t < num_threads; ++t) {
      workers.emplace_back([&ocr, &all_matched]() {
        sublift::ImageBuffer buf(50, 20, sublift::PixelFormat::RGB24);
        for (int i = 0; i < calls_per_thread; ++i) {
          auto res = ocr.recognize(buf.view());
          if (res.text != "Concurrent OCR") {
            all_matched.store(false, std::memory_order_relaxed);
          }
        }
      });
    }

    for (auto& th : workers) {
      th.join();
    }

    REQUIRE(all_matched.load());
    REQUIRE(ocr.call_count() == static_cast<std::size_t>(num_threads * calls_per_thread));
  }

  SECTION("MockSubtitleSink emit and lifecycle") {
    sublift::adapters::mock::MockSubtitleSink sink;
    REQUIRE(sink.entries().empty());
    REQUIRE_FALSE(sink.is_flushed());
    REQUIRE_FALSE(sink.is_closed());

    sink.emit(sublift::SubtitleEntry{
        .id = 1,
        .range = sublift::TimeRange{1000, 2000},
        .text = "Mock subtitle 1",
        .confidence = 0.95,
    });
    sink.emit(sublift::SubtitleEntry{
        .id = 2,
        .range = sublift::TimeRange{3000, 4500},
        .text = "Mock subtitle 2",
        .confidence = 0.88,
    });

    REQUIRE(sink.entries().size() == 2);
    REQUIRE(sink.entries()[0].text == "Mock subtitle 1");
    REQUIRE(sink.entries()[1].text == "Mock subtitle 2");

    sink.flush();
    REQUIRE(sink.is_flushed());

    sink.close();
    REQUIRE(sink.is_closed());

    sink.clear();
    REQUIRE(sink.entries().empty());
    REQUIRE_FALSE(sink.is_flushed());
    REQUIRE_FALSE(sink.is_closed());
  }

  SECTION("MockSubtitleSink multi-threaded concurrent emission and snapshot") {
    sublift::adapters::mock::MockSubtitleSink sink;
    constexpr int num_threads = 8;
    constexpr int entries_per_thread = 50;
    std::vector<std::thread> workers;
    workers.reserve(num_threads);

    for (int t = 0; t < num_threads; ++t) {
      workers.emplace_back([&sink, t]() {
        for (int i = 0; i < entries_per_thread; ++i) {
          sink.emit(sublift::SubtitleEntry{
              .id = static_cast<std::uint64_t>(t * entries_per_thread + i),
              .range = sublift::TimeRange{i * 100, i * 100 + 80},
              .text = "Thread " + std::to_string(t) + " entry " + std::to_string(i),
              .confidence = 0.9,
          });
        }
      });
    }

    for (auto& th : workers) {
      th.join();
    }

    auto snapshot = sink.entries();
    REQUIRE(snapshot.size() == static_cast<std::size_t>(num_threads * entries_per_thread));
    REQUIRE(sink.size() == static_cast<std::size_t>(num_threads * entries_per_thread));
    REQUIRE_FALSE(sink.empty());

    sink.flush();
    REQUIRE(sink.is_flushed());

    sink.close();
    REQUIRE(sink.is_closed());

    sink.clear();
    REQUIRE(sink.empty());
    REQUIRE(sink.entries().empty());
  }

  SECTION("MockFrameSource pre-planned frames and concurrent cancellation") {
    sublift::adapters::mock::MockFrameSource source;
    constexpr int total_planned = 50;
    for (int i = 0; i < total_planned; ++i) {
      source.add_frame(sublift::Frame{
          .timestamp_ms = i * 40,
          .image = sublift::ImageBuffer(80, 30, sublift::PixelFormat::RGB24),
      });
    }
    REQUIRE(source.frame_count() == total_planned);

    std::atomic<int> frames_received{0};
    std::thread extract_th([&source, &frames_received]() {
      source.extract("test.mp4", 25.0, std::nullopt, [&](sublift::Frame) {
        frames_received.fetch_add(1, std::memory_order_relaxed);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        return true;
      });
    });

    while (frames_received.load(std::memory_order_relaxed) < 3) {
      std::this_thread::yield();
    }

    source.cancel();
    extract_th.join();

    REQUIRE(source.is_cancelled());
    REQUIRE(frames_received.load() < total_planned);
  }
}
