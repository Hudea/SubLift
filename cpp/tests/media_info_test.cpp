#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <thread>
#include <cstdlib>
#include <vector>
#include <unistd.h>

#include <sublift/adapters/ffmpeg/ffmpeg_frame_source.hpp>
#include "../adapters/ffmpeg/src/process_utils.hpp"

using namespace sublift;
using namespace sublift::adapters::ffmpeg;

namespace {
struct MediaInfoFixture {
  std::filesystem::path root;
  MediaInfoFixture() {
    auto pattern = (std::filesystem::temp_directory_path() / "sublift_info_XXXXXX").string();
    if (!::mkdtemp(pattern.data())) throw std::runtime_error("Cannot create source info fixture");
    root = pattern;
  }
  ~MediaInfoFixture() {
    std::error_code error;
    std::filesystem::remove_all(root, error);
  }
};
}  // namespace

TEST_CASE("Source metadata reports original geometry and duration", "[extractor][video_info]") {
  MediaInfoFixture fix;

  SECTION("Missing and corrupt paths fail without guessing dimensions") {
    const auto missing = fix.root / "nonexistent.mp4";
    REQUIRE_THROWS_AS(probe_source_frame(missing), std::runtime_error);
    CHECK(probe_duration_ms(missing) == 0);

    const auto corrupt = fix.root / "corrupt.mp4";
    std::ofstream(corrupt) << "broken";
    REQUIRE_THROWS_AS(probe_source_frame(corrupt), std::runtime_error);
    CHECK(probe_duration_ms(corrupt) == 0);
  }

  SECTION("Rotated media metadata uses the same decoded geometry as its preview frame") {
    if (!available()) SKIP("FFmpeg is not installed");
    const auto base = fix.root / "base.mp4";
    const auto video = fix.root / "rotated.mp4";
    const auto ffmpeg = resolve_ffmpeg_bin();
    const auto encoded = detail::run_subprocess(
        {ffmpeg, "-v", "error", "-y", "-f", "lavfi", "-i",
         "testsrc2=size=80x48:rate=10:duration=0.3", "-pix_fmt", "yuv420p", base.string()},
        std::chrono::seconds(10));
    REQUIRE(encoded.exit_code == 0);

    const auto rotate = detail::run_subprocess(
        {ffmpeg, "-v", "error", "-y", "-display_rotation", "90", "-i", base.string(),
         "-c", "copy", video.string()},
        std::chrono::seconds(10));
    if (rotate.exit_code != 0) SKIP("FFmpeg lacks display_rotation fixture option");

    const auto source = probe_source_frame(video);
    CHECK(source.width == 48);
    CHECK(source.height == 80);
    CHECK(source.display_transform_ok == true);

    const auto rgb = extract_single_frame_rgb24(video, 0.0);
    CHECK(rgb.width() == 48);
    CHECK(rgb.height() == 80);
    REQUIRE_FALSE(rgb.empty());
  }

  SECTION("Non-square pixels and rotation expose correct pixel aspect without changing pixel coordinates") {
    if (!available()) SKIP("FFmpeg is not installed");
    const auto ffmpeg = resolve_ffmpeg_bin();
    const auto base = fix.root / "sar.mp4";
    const auto rotated = fix.root / "sar-rotated.mp4";
    const auto encoded = detail::run_subprocess(
        {ffmpeg, "-v", "error", "-y", "-f", "lavfi", "-i",
         "testsrc2=size=80x48:rate=10:duration=0.3", "-vf", "setsar=4/3",
         "-pix_fmt", "yuv420p", base.string()},
        std::chrono::seconds(10));
    REQUIRE(encoded.exit_code == 0);

    const auto original = probe_source_frame(base);
    CHECK(original.width == 80);
    CHECK(original.height == 48);
    CHECK(original.pixel_aspect_num == 4);
    CHECK(original.pixel_aspect_den == 3);

    const auto turn = detail::run_subprocess(
        {ffmpeg, "-v", "error", "-y", "-display_rotation", "90", "-i", base.string(),
         "-c", "copy", rotated.string()},
        std::chrono::seconds(10));
    if (turn.exit_code != 0) SKIP("FFmpeg lacks display_rotation fixture option");

    const auto rotated_info = probe_source_frame(rotated);
    CHECK(rotated_info.width == 48);
    CHECK(rotated_info.height == 80);
    CHECK(rotated_info.pixel_aspect_num == 3);
    CHECK(rotated_info.pixel_aspect_den == 4);
  }

  SECTION("Non-zero starts use elapsed duration and format_start_ms") {
    if (!available()) SKIP("FFmpeg is not installed");
    const auto ffmpeg = resolve_ffmpeg_bin();
    const auto mkv = fix.root / "shifted.mkv";
    const auto mp4 = fix.root / "shifted.mp4";
    const auto ts = fix.root / "source.m2ts";

    const auto generate = [&](const std::filesystem::path& path, const std::string& codec, bool offset) {
      std::vector<std::string> args{
          ffmpeg, "-v", "error", "-y", "-f", "lavfi", "-i",
          "testsrc2=size=80x48:rate=10:duration=1", "-c:v", codec, "-pix_fmt", "yuv420p"};
      if (offset) {
        args.insert(args.end(), {"-output_ts_offset", "10"});
      }
      args.push_back(path.string());
      return detail::run_subprocess(args, std::chrono::seconds(15));
    };

    REQUIRE(generate(mkv, "mpeg4", true).exit_code == 0);
    REQUIRE(generate(mp4, "libx264", true).exit_code == 0);
    REQUIRE(generate(ts, "mpeg2video", false).exit_code == 0);

    for (const auto& file : {mkv, mp4, ts}) {
      CAPTURE(file.string());
      const auto info = probe_source_and_duration(file);
      CHECK(info.format_start_ms > 1000);
      CHECK(info.duration_ms >= 900);
      CHECK(info.duration_ms <= 1100);
      CHECK(probe_duration_ms(file) == info.duration_ms);
    }
  }

  SECTION("Real source dimensions and duration from probe_video") {
    if (!available()) SKIP("FFmpeg is not installed");
    const auto video = fix.root / "source.mp4";
    const auto encoded = detail::run_subprocess(
        {resolve_ffmpeg_bin(), "-v", "error", "-y", "-f", "lavfi", "-i",
         "color=c=black:s=80x48:r=10", "-t", "0.2", "-pix_fmt", "yuv420p", video.string()},
        std::chrono::seconds(10));
    REQUIRE(encoded.exit_code == 0);

    const auto vinfo = probe_video(video);
    CHECK(vinfo.width == 80);
    CHECK(vinfo.height == 48);
    CHECK(vinfo.duration_ms >= 100);
    CHECK(vinfo.duration_ms <= 300);

    const auto frame = extract_single_frame_rgb24(video, 0.0);
    CHECK(frame.width() == 80);
    CHECK(frame.height() == 48);
  }
}
