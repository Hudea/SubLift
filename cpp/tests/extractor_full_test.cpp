#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>

#include "../src/ffmpeg/process_utils.hpp"
#include "sublift/ffmpeg.hpp"

using namespace sublift;
using namespace sublift::ffmpeg;

TEST_CASE("FfmpegExtractor full extraction and cancel tests", "[extractor][full]") {
  if (!available()) {
    WARN("ffmpeg/ffprobe not available; skipping FfmpegExtractor integration tests");
    return;
  }

  const std::filesystem::path tmp_video =
      std::filesystem::temp_directory_path() / "sublift_test_extractor_full.mp4";
  const std::filesystem::path tmp_corrupt =
      std::filesystem::temp_directory_path() / "sublift_test_extractor_corrupt.bin";

  std::error_code ec;
  std::filesystem::remove(tmp_video, ec);
  std::filesystem::remove(tmp_corrupt, ec);

  // Generate 3-second synthetic video at 1 fps (320x240)
  const std::string ffmpeg_bin = resolve_ffmpeg_bin();
  const std::vector<std::string> gen_cmd = {
      ffmpeg_bin,
      "-nostdin",
      "-v",
      "error",
      "-f",
      "lavfi",
      "-i",
      "testsrc=size=320x240:rate=1",
      "-t",
      "3",
      "-pix_fmt",
      "yuv420p",
      "-y",
      tmp_video.string(),
  };

  auto gen_res = detail::run_subprocess(gen_cmd, std::chrono::milliseconds(15000));
  REQUIRE(gen_res.exit_code == 0);
  REQUIRE(std::filesystem::exists(tmp_video));

  SECTION("Constructor invalid fps check") {
    REQUIRE_THROWS_AS(FfmpegExtractor(0.0), std::invalid_argument);
    REQUIRE_THROWS_AS(FfmpegExtractor(-1.0), std::invalid_argument);
    REQUIRE_THROWS_AS(FfmpegExtractor(std::numeric_limits<double>::quiet_NaN()), std::invalid_argument);
  }

  SECTION("Full extraction (1 fps, 3 sec -> 3 frames, [0, 1000, 2000])") {
    FfmpegExtractor extractor(1.0);
    std::vector<std::int64_t> timestamps;
    std::vector<SourceBox> dims;

    extractor.extract(tmp_video, [&](Frame frame) {
      timestamps.push_back(frame.timestamp_ms);
      dims.push_back(SourceBox{0, 0, frame.image.width(), frame.image.height()});
      REQUIRE(frame.image.format() == PixelFormat::RGB24);
      REQUIRE(frame.image.data() != nullptr);
      return true;
    });

    REQUIRE(timestamps.size() == 3);
    REQUIRE(timestamps == std::vector<std::int64_t>{0, 1000, 2000});
    REQUIRE(dims[0] == SourceBox{0, 0, 320, 240});
  }

  SECTION("ROI crop extraction (crop 160x120 at 10,10)") {
    SourceBox crop{10, 10, 160, 120};
    FfmpegExtractor extractor(1.0, crop);
    std::vector<SourceBox> dims;

    extractor.extract(tmp_video, [&](Frame frame) {
      dims.push_back(SourceBox{0, 0, frame.image.width(), frame.image.height()});
      return true;
    });

    REQUIRE(dims.size() == 3);
    REQUIRE(dims[0] == SourceBox{0, 0, 160, 120});
  }

  SECTION("Invalid crop out of bounds throws std::invalid_argument") {
    SourceBox invalid_crop{0, 0, 1000, 1000};
    FfmpegExtractor extractor(1.0, invalid_crop);
    REQUIRE_THROWS_AS(extractor.extract(tmp_video, [](Frame) { return true; }),
                      std::invalid_argument);
  }

  SECTION("FPS timestamp parity check (2.0 fps 2.5s -> [0, 500, 1000, 1500, 2000])") {
    FfmpegExtractor extractor(2.0);
    std::vector<std::int64_t> timestamps;

    extractor.extract(tmp_video, [&](Frame frame) {
      timestamps.push_back(frame.timestamp_ms);
      return true;
    });

    // 3 sec at 2 fps yields 6 frames: 0, 500, 1000, 1500, 2000, 2500
    REQUIRE(timestamps.size() == 6);
    REQUIRE(timestamps == std::vector<std::int64_t>{0, 500, 1000, 1500, 2000, 2500});
  }

  SECTION("Pre-cancelled extraction") {
    FfmpegExtractor extractor(1.0);
    extractor.cancel();
    REQUIRE(extractor.is_cancelled());

    int count = 0;
    REQUIRE_NOTHROW(extractor.extract(tmp_video, [&](Frame) {
      count++;
      return true;
    }));

    REQUIRE(count == 0);
  }

  SECTION("Mid-flight multithreaded cancellation") {
    // Generate 10-second video for cancellation test
    const std::filesystem::path long_video =
        std::filesystem::temp_directory_path() / "sublift_test_extractor_long.mp4";
    const std::vector<std::string> long_gen_cmd = {
        ffmpeg_bin,
        "-nostdin",
        "-v",
        "error",
        "-f",
        "lavfi",
        "-i",
        "testsrc=size=320x240:rate=10",
        "-t",
        "10",
        "-pix_fmt",
        "yuv420p",
        "-y",
        long_video.string(),
    };
    static_cast<void>(detail::run_subprocess(long_gen_cmd, std::chrono::milliseconds(15000)));

    FfmpegExtractor extractor(10.0);
    int frame_count = 0;

    auto t0 = std::chrono::steady_clock::now();
    std::thread extract_thread([&]() {
      extractor.extract(long_video, [&](Frame) {
        frame_count++;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return true;
      });
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    extractor.cancel();

    extract_thread.join();
    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0).count();

    REQUIRE(elapsed_ms < 1000);  // Should finish cleanly in < 1 second
    REQUIRE(frame_count < 100);  // Should not extract full 100 frames

    std::filesystem::remove(long_video, ec);
  }

  SECTION("Non-existent file throws std::runtime_error") {
    FfmpegExtractor extractor(1.0);
    std::filesystem::path fake_path{"/tmp/sublift_non_existent_file_777.mp4"};
    try {
      extractor.extract(fake_path, [](Frame) { return true; });
      FAIL("Should have thrown std::runtime_error");
    } catch (const std::runtime_error& exc) {
      std::string msg = exc.what();
      REQUIRE(msg.find("视频文件不存在") != std::string::npos);
    }
  }

  SECTION("Corrupt file error diagnostic tail") {
    {
      std::ofstream ofs(tmp_corrupt, std::ios::binary);
      ofs << "GARBAGE_BYTES_FOR_CORRUPT_TEST";
    }

    FfmpegExtractor extractor(1.0);
    try {
      extractor.extract(tmp_corrupt, [](Frame) { return true; });
      FAIL("Should have thrown std::runtime_error");
    } catch (const std::runtime_error& exc) {
      std::string msg = exc.what();
      REQUIRE((msg.find("ffprobe 失败") != std::string::npos ||
               msg.find("ffmpeg 抽帧失败") != std::string::npos));
    }
  }

  SECTION("Consumer early termination (returns false on frame 2)") {
    FfmpegExtractor extractor(1.0);
    int count = 0;
    extractor.extract(tmp_video, [&](Frame) {
      count++;
      return count < 2;  // Stop on second frame
    });

    REQUIRE(count == 2);
  }

  SECTION("Consumer exception safety") {
    FfmpegExtractor extractor(1.0);
    int count = 0;
    try {
      extractor.extract(tmp_video, [&](Frame) -> bool {
        count++;
        if (count == 1) {
          throw std::logic_error("Consumer test exception");
        }
        return true;
      });
      FAIL("Should have rethrown logic_error");
    } catch (const std::logic_error& exc) {
      REQUIRE(std::string(exc.what()) == "Consumer test exception");
    }
    REQUIRE(count == 1);
  }

  // Cleanup
  std::filesystem::remove(tmp_video, ec);
  std::filesystem::remove(tmp_corrupt, ec);
}
