#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

#include "../src/ffmpeg/process_utils.hpp"
#include "sublift/ffmpeg.hpp"

using namespace sublift;
using namespace sublift::ffmpeg;

TEST_CASE("read_stderr_tail string formatting", "[extractor][probe][pure]") {
  SECTION("Empty string") {
    REQUIRE(detail::read_stderr_tail("") == "");
  }

  SECTION("Short single line") {
    REQUIRE(detail::read_stderr_tail("  error: failed to open file  ") ==
            "error: failed to open file");
  }

  SECTION("Multiline and whitespace collapse") {
    std::string input = "  line1  \n\t  line2 \r\n   line3   ";
    REQUIRE(detail::read_stderr_tail(input) == "line1 line2 line3");
  }

  SECTION("Truncation to max_chars") {
    std::string long_input(3000, 'a');
    std::string tail = detail::read_stderr_tail(long_input, 2000);
    REQUIRE(tail.size() == 2000);
  }
}

TEST_CASE("resolve_bin and available checks", "[extractor][probe][pure]") {
  SECTION("resolve_ffmpeg_bin and resolve_ffprobe_bin") {
    if (available()) {
      REQUIRE_NOTHROW(resolve_ffmpeg_bin());
      REQUIRE_NOTHROW(resolve_ffprobe_bin());
      REQUIRE(resolve_ffmpeg_bin().find("ffmpeg") != std::string::npos);
      REQUIRE(resolve_ffprobe_bin().find("ffprobe") != std::string::npos);
    }
  }

  SECTION("probe_duration_ms on non-existent file returns 0") {
    std::filesystem::path fake_path{"/tmp/sublift_non_existent_file_98765.mp4"};
    REQUIRE(probe_duration_ms(fake_path) == 0);
  }

  SECTION("probe_source_frame on non-existent file throws std::runtime_error") {
    std::filesystem::path fake_path{"/tmp/sublift_non_existent_file_98765.mp4"};
    REQUIRE_THROWS_AS(probe_source_frame(fake_path), std::runtime_error);
  }
}

TEST_CASE("ffprobe integration test with synthetic video", "[extractor][probe][integration]") {
  if (!available()) {
    WARN("ffmpeg/ffprobe not available in environment; skipping integration test");
    return;
  }

  const std::filesystem::path tmp_video =
      std::filesystem::temp_directory_path() / "sublift_test_synthetic.mp4";
  const std::filesystem::path tmp_corrupt =
      std::filesystem::temp_directory_path() / "sublift_test_corrupt.bin";

  std::error_code ec;
  std::filesystem::remove(tmp_video, ec);
  std::filesystem::remove(tmp_corrupt, ec);

  // Generate synthetic 2-second video with ffmpeg
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
      "2",
      "-pix_fmt",
      "yuv420p",
      "-y",
      tmp_video.string(),
  };

  auto gen_res = detail::run_subprocess(gen_cmd, std::chrono::milliseconds(15000));
  REQUIRE(gen_res.exit_code == 0);
  REQUIRE(std::filesystem::exists(tmp_video));

  SECTION("probe_source_frame on synthetic video") {
    auto info = probe_source_frame(tmp_video);
    REQUIRE(info.width == 320);
    REQUIRE(info.height == 240);
    REQUIRE(info.display_transform_ok);
    REQUIRE_FALSE(info.transform_note.has_value());
  }

  SECTION("probe_duration_ms on synthetic video") {
    auto dur_ms = probe_duration_ms(tmp_video);
    REQUIRE(dur_ms >= 1800);
    REQUIRE(dur_ms <= 2200);
  }

  SECTION("probe_video on synthetic video") {
    auto video_info = probe_video(tmp_video);
    REQUIRE(video_info.width == 320);
    REQUIRE(video_info.height == 240);
    REQUIRE(video_info.duration_ms >= 1800);
    REQUIRE(video_info.duration_ms <= 2200);
  }

  SECTION("probe_source_frame on corrupt video file throws std::runtime_error with stderr tail") {
    {
      std::ofstream ofs(tmp_corrupt, std::ios::binary);
      ofs << "NOT_A_VIDEO_FILE_HEADER_GARBAGE_BYTES_1234567890";
    }

    try {
      static_cast<void>(probe_source_frame(tmp_corrupt));
      FAIL("Should have thrown std::runtime_error for corrupt file");
    } catch (const std::runtime_error& exc) {
      std::string msg = exc.what();
      REQUIRE(msg.find("ffprobe 失败") != std::string::npos);
      REQUIRE(msg.find(tmp_corrupt.string()) != std::string::npos);
    }
  }

  // Cleanup
  std::filesystem::remove(tmp_video, ec);
  std::filesystem::remove(tmp_corrupt, ec);
}
