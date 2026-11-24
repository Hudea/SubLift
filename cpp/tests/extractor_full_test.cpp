#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <thread>
#include <vector>
#include <unistd.h>

#include <sublift/adapters/ffmpeg/ffmpeg_frame_source.hpp>
#include "../adapters/ffmpeg/src/process_utils.hpp"

using namespace sublift;
using namespace sublift::adapters::ffmpeg;

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
    REQUIRE_THROWS_AS(FfmpegFrameSource(0.0), std::invalid_argument);
    REQUIRE_THROWS_AS(FfmpegFrameSource(-1.0), std::invalid_argument);
    REQUIRE_THROWS_AS(FfmpegFrameSource(std::numeric_limits<double>::quiet_NaN()), std::invalid_argument);
  }

  SECTION("Full extraction (1 fps, 3 sec -> 3 frames, [0, 1000, 2000])") {
    FfmpegFrameSource extractor(1.0);
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
    FfmpegFrameSource extractor(1.0, crop);
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
    FfmpegFrameSource extractor(1.0, invalid_crop);
    REQUIRE_THROWS_AS(extractor.extract(tmp_video, [](Frame) { return true; }),
                      std::invalid_argument);
  }

  SECTION("FPS timestamp parity check (2.0 fps 2.5s -> [0, 500, 1000, 1500, 2000])") {
    FfmpegFrameSource extractor(2.0);
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
    FfmpegFrameSource extractor(1.0);
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

    FfmpegFrameSource extractor(10.0);
    int frame_count = 0;

    std::promise<void> first_frame_ready;
    auto ready_future = first_frame_ready.get_future();
    bool notified = false;
    std::thread extract_thread([&]() {
      extractor.extract(long_video, [&](Frame) {
        frame_count++;
        if (!notified) {
          notified = true;
          first_frame_ready.set_value();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        return true;
      });
    });

    REQUIRE(ready_future.wait_for(std::chrono::seconds(10)) == std::future_status::ready);
    extractor.cancel();

    extract_thread.join();

    REQUIRE(frame_count >= 1);
    REQUIRE(frame_count < 100);  // Should not extract full 100 frames

    std::filesystem::remove(long_video, ec);
  }

  SECTION("Non-existent file throws std::runtime_error") {
    FfmpegFrameSource extractor(1.0);
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

    FfmpegFrameSource extractor(1.0);
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
    FfmpegFrameSource extractor(1.0);
    int count = 0;
    extractor.extract(tmp_video, [&](Frame) {
      count++;
      return count < 2;  // Stop on second frame
    });

    REQUIRE(count == 2);
  }

  SECTION("Consumer exception safety") {
    FfmpegFrameSource extractor(1.0);
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

  SECTION("Hardware acceleration option and automatic software fallback") {
    FfmpegFrameSource hw_extractor(1.0, std::nullopt, std::nullopt, std::string("videotoolbox"));
    REQUIRE(hw_extractor.hwaccel() == std::optional<std::string>("videotoolbox"));
    std::vector<std::int64_t> hw_ts;
    hw_extractor.extract(tmp_video, [&](Frame frame) {
      hw_ts.push_back(frame.timestamp_ms);
      return true;
    });
    REQUIRE(hw_ts == std::vector<std::int64_t>{0, 1000, 2000});

    FfmpegFrameSource fallback_extractor(
        1.0, std::nullopt, std::nullopt, std::string("nonexistent_hwaccel_xyz"));
    std::vector<std::int64_t> fb_ts;
    fallback_extractor.extract(tmp_video, [&](Frame frame) {
      fb_ts.push_back(frame.timestamp_ms);
      return true;
    });
    REQUIRE(fb_ts == std::vector<std::int64_t>{0, 1000, 2000});
  }

  SECTION("Time range extraction and validation") {
    FfmpegFrameSource range_extractor(1.0);
    REQUIRE_THROWS_AS(range_extractor.set_time_range(-1.0, 1.0), std::invalid_argument);
    REQUIRE_THROWS_AS(range_extractor.set_time_range(0.0, 0.0), std::invalid_argument);
    REQUIRE_THROWS_AS(range_extractor.set_time_range(0.0, -2.0), std::invalid_argument);

    range_extractor.set_time_range(1.0, 1.0);
    REQUIRE(range_extractor.start_sec() == std::optional<double>(1.0));
    REQUIRE(range_extractor.duration_sec() == std::optional<double>(1.0));

    std::vector<std::int64_t> range_ts;
    range_extractor.extract(tmp_video, [&](Frame frame) {
      range_ts.push_back(frame.timestamp_ms);
      return true;
    });
    REQUIRE(range_ts == std::vector<std::int64_t>{1000});
  }

  // Cleanup
  std::filesystem::remove(tmp_video, ec);
  std::filesystem::remove(tmp_corrupt, ec);
}

TEST_CASE("Orthogonal display matrices preserve decoded ROI geometry", "[extractor][rotation][integration]") {
  if (!available()) SKIP("ffmpeg/ffprobe not available");
  auto root_pattern = (std::filesystem::temp_directory_path() / "sublift_rotation_XXXXXX").string();
  REQUIRE(::mkdtemp(root_pattern.data()) != nullptr);
  struct Fixture {
    std::filesystem::path root;
    ~Fixture() { std::error_code ec; std::filesystem::remove_all(root, ec); }
  } fixture{root_pattern};
  const auto ffmpeg = resolve_ffmpeg_bin();
  const auto base = fixture.root / "base.mp4";
  const auto generated = detail::run_subprocess({ffmpeg,"-v","error","-y","-f","lavfi","-i",
      "testsrc2=size=80x48:rate=10:duration=0.4","-pix_fmt","yuv420p",base.string()}, std::chrono::seconds(15));
  REQUIRE(generated.exit_code == 0);
  for (int angle : {90,180,270}) {
    const auto video = fixture.root / (std::to_string(angle)+".mp4");
    const auto rotated = detail::run_subprocess({ffmpeg,"-v","error","-y","-display_rotation",
        std::to_string(angle),"-i",base.string(),"-c","copy",video.string()}, std::chrono::seconds(15));
    if (rotated.exit_code != 0) SKIP("FFmpeg lacks the display_rotation fixture option");
    const auto source = probe_source_frame(video);
    CAPTURE(angle);
    REQUIRE(source.display_transform_ok);
    const int width = angle == 180 ? 80 : 48, height = angle == 180 ? 48 : 80;
    REQUIRE(source.width == width); REQUIRE(source.height == height);
    const auto combined = probe_source_and_duration(video);
    REQUIRE(combined.source == source);
    const SourceBox box{5,7,20,14};
    const auto plan = plan_frame_io(video,box,"roi");
    REQUIRE(plan.output_crop == box);
    std::vector<std::uint8_t> full, cropped;
    FfmpegFrameSource full_extractor(5.0);
    full_extractor.extract(video,[&](Frame frame) {
      if (full.empty()) {
        REQUIRE(frame.image.width() == width); REQUIRE(frame.image.height() == height);
        for(int y=0;y<height;++y) full.insert(full.end(),frame.image.data()+y*frame.image.stride_bytes(),frame.image.data()+y*frame.image.stride_bytes()+width*3);
      }
      return true;
    });
    FfmpegFrameSource roi_extractor(5.0,box);
    roi_extractor.extract(video,[&](Frame frame) {
      if (cropped.empty()) {
        REQUIRE(frame.image.width() == box.width); REQUIRE(frame.image.height() == box.height);
        for(int y=0;y<box.height;++y) cropped.insert(cropped.end(),frame.image.data()+y*frame.image.stride_bytes(),frame.image.data()+y*frame.image.stride_bytes()+box.width*3);
      }
      return true;
    });
    REQUIRE(full.size() == static_cast<std::size_t>(width*height*3));
    REQUIRE(cropped.size() == static_cast<std::size_t>(box.width*box.height*3));
    for(int y=0;y<box.height;++y)
      REQUIRE(std::memcmp(cropped.data()+y*box.width*3,full.data()+((y+box.y)*width+box.x)*3,box.width*3)==0);
    const auto single = extract_single_frame_rgb24(video,0.0);
    REQUIRE(single.width() == width); REQUIRE(single.height() == height);
  }
  const auto mirrored=fixture.root/"mirrored.mp4";
  const auto flip=detail::run_subprocess({ffmpeg,"-v","error","-y","-display_rotation","0","-display_hflip",
      "-i",base.string(),"-c","copy",mirrored.string()},std::chrono::seconds(15));
  REQUIRE(flip.exit_code==0);
  const auto source=probe_source_frame(mirrored);REQUIRE_FALSE(source.display_transform_ok);
  FfmpegFrameSource extractor(5.0);
  REQUIRE_THROWS_WITH(extractor.extract(mirrored,[](Frame){return true;}), Catch::Matchers::ContainsSubstring("显示变换"));
  REQUIRE_THROWS_AS(extract_single_frame_rgb24(mirrored,0.0),std::runtime_error);

  const auto vertical=fixture.root/"vertical-mirror.mp4";
  const auto vflip=detail::run_subprocess({ffmpeg,"-v","error","-y","-display_rotation","0","-display_vflip",
      "-i",base.string(),"-c","copy",vertical.string()},std::chrono::seconds(15));
  REQUIRE(vflip.exit_code==0);
  REQUIRE_FALSE(probe_source_frame(vertical).display_transform_ok);
  FfmpegFrameSource vertical_extractor(5.0);
  REQUIRE_THROWS_AS(vertical_extractor.extract(vertical,[](Frame){return true;}),std::runtime_error);
}

TEST_CASE("Extractor preserves a video stream that begins after audio", "[extractor][timing][integration]") {
  if (!available()) SKIP("ffmpeg/ffprobe unavailable");
  auto root_pattern = (std::filesystem::temp_directory_path() / "sublift_late_video_XXXXXX").string();
  REQUIRE(::mkdtemp(root_pattern.data()) != nullptr);
  struct Fixture { std::filesystem::path root; ~Fixture(){std::error_code ec;std::filesystem::remove_all(root,ec);} } fixture{root_pattern};
  const auto video=fixture.root/"late.mkv";const auto ffmpeg=resolve_ffmpeg_bin();
  const auto result=detail::run_subprocess({ffmpeg,"-v","error","-y","-f","lavfi","-i",
    "sine=frequency=440:sample_rate=44100:duration=5","-itsoffset","2","-f","lavfi","-i",
    "testsrc2=size=80x48:rate=10:duration=3","-map","1:v","-map","0:a","-c:v","mpeg4","-c:a","aac",video.string()},std::chrono::seconds(15));
  REQUIRE(result.exit_code==0);
  const auto probed=probe_source_frame(video);const auto combined=probe_source_and_duration(video);
  REQUIRE(probed.relative_video_start_ms==2000);REQUIRE(combined.source.relative_video_start_ms==2000);
  auto check=[&](double start,double duration,std::int64_t first,std::int64_t last,std::size_t count){
    FfmpegFrameSource extractor(5.0);if(start>0)extractor.set_time_range(start,duration);
    std::vector<std::int64_t> times;extractor.extract(video,[&](Frame frame){times.push_back(frame.timestamp_ms);return true;});
    REQUIRE(times.size()==count);CHECK(times.front()==first);CHECK(times.back()==last);
  };
  check(0,0,2000,4800,15);check(1,1.5,2000,2400,3);check(2.5,0.8,2500,3100,4);
}

TEST_CASE("Two-stage crop produces byte-identical RGB24 frames vs legacy single-stage filter",
          "[extractor][two_stage][integration]") {
  if (!available()) SKIP("ffmpeg/ffprobe unavailable");

  auto root_pattern =
      (std::filesystem::temp_directory_path() / "sublift_twostage_XXXXXX").string();
  REQUIRE(::mkdtemp(root_pattern.data()) != nullptr);
  struct Fixture {
    std::filesystem::path root;
    ~Fixture() {
      std::error_code ec;
      std::filesystem::remove_all(root, ec);
    }
  } fixture{root_pattern};

  const auto ffmpeg = resolve_ffmpeg_bin();

  auto collect_bytes = [](FfmpegFrameSource& ext, const std::filesystem::path& video) {
    std::vector<std::uint8_t> out;
    ext.extract(video, [&](Frame frame) {
      const std::size_t row_bytes = static_cast<std::size_t>(frame.image.width()) * 3;
      const auto view = frame.image.view();
      for (int y = 0; y < view.height(); ++y) {
        const auto* row = view.row(y);
        out.insert(out.end(), row, row + row_bytes);
      }
      return true;
    });
    return out;
  };

  // Orthogonal rawvideo matrix preserving 8/10-bit planar, semi-planar, VideoToolbox,
  // odd/corner/SIMD alignments, and non-16-aligned dimensions in minimal wall time.
  auto verify_crops = [&](const std::filesystem::path& video,
                          const std::string& label,
                          const std::optional<std::string>& hwaccel,
                          const std::vector<SourceBox>& crops) {
    const auto src_info = probe_source_frame(video);
    for (const auto& crop : crops) {
      CAPTURE(label, crop.x, crop.y, crop.width, crop.height);
      FfmpegFrameSource legacy_ext(
          5.0, crop, src_info, hwaccel,
          FfmpegExtractorOptions{.enable_two_stage_crop = false});
      FfmpegFrameSource opt_ext(
          5.0, crop, src_info, hwaccel,
          FfmpegExtractorOptions{.enable_two_stage_crop = true});

      const auto legacy_bytes = collect_bytes(legacy_ext, video);
      const auto opt_bytes = collect_bytes(opt_ext, video);
      REQUIRE_FALSE(legacy_bytes.empty());
      REQUIRE(opt_bytes.size() == legacy_bytes.size());
      REQUIRE(std::memcmp(legacy_bytes.data(), opt_bytes.data(), legacy_bytes.size()) == 0);
    }
  };

  const SourceBox full_width_odd_yh{0, 779, 1920, 121};
  const SourceBox odd_xw_simd{333, 777, 1001, 123};
  const SourceBox br_corner{1905, 1055, 15, 25};

  // 1. 1920x1080 8-bit and 10-bit planar YUV (rawvideo in .nut)
  for (const auto& pix_fmt : {std::string("yuv420p"), std::string("yuv420p10le")}) {
    const auto video = fixture.root / ("hd_" + pix_fmt + ".nut");
    const auto gen = detail::run_subprocess(
        {ffmpeg, "-v", "error", "-y", "-f", "lavfi", "-i",
         "testsrc2=size=1920x1080:rate=5:duration=0.2", "-pix_fmt", pix_fmt,
         "-c:v", "rawvideo", video.string()},
        std::chrono::seconds(20));
    REQUIRE(gen.exit_code == 0);
    verify_crops(video, pix_fmt, std::string("none"),
                 {full_width_odd_yh, odd_xw_simd, br_corner});
  }

  // 2. Additional chroma subsampling & semi-planar formats (yuv422p, yuv444p, nv12, p010le)
  for (const auto& pix_fmt :
       {std::string("yuv422p"), std::string("yuv444p"),
        std::string("nv12"), std::string("p010le")}) {
    const auto video = fixture.root / ("hd_fmt_" + pix_fmt + ".nut");
    const auto gen = detail::run_subprocess(
        {ffmpeg, "-v", "error", "-y", "-f", "lavfi", "-i",
         "testsrc2=size=1920x1080:rate=5:duration=0.2", "-pix_fmt", pix_fmt,
         "-c:v", "rawvideo", video.string()},
        std::chrono::seconds(20));
    REQUIRE(gen.exit_code == 0);
    verify_crops(video, pix_fmt, std::string("none"), {odd_xw_simd});
  }

#ifdef __APPLE__
  // 3. Hardware-accelerated VideoToolbox H.264 decode
  {
    const auto h264_mp4 = fixture.root / "hd_vt_h264.mp4";
    const auto gen_mp4 = detail::run_subprocess(
        {ffmpeg, "-v", "error", "-y", "-f", "lavfi", "-i",
         "testsrc2=size=1920x1080:rate=5:duration=0.2", "-pix_fmt", "yuv420p",
         "-c:v", "libx264", "-crf", "18", h264_mp4.string()},
        std::chrono::seconds(20));
    REQUIRE(gen_mp4.exit_code == 0);
    verify_crops(h264_mp4, "videotoolbox", std::string("videotoolbox"),
                 {full_width_odd_yh, odd_xw_simd});
  }
#endif

  // 4. Non-16-multiple source width (854x480) in 10-bit
  {
    const auto video_480p = fixture.root / "sd854_yuv420p10le.nut";
    const auto gen = detail::run_subprocess(
        {ffmpeg, "-v", "error", "-y", "-f", "lavfi", "-i",
         "testsrc2=size=854x480:rate=5:duration=0.2", "-pix_fmt", "yuv420p10le",
         "-c:v", "rawvideo", video_480p.string()},
        std::chrono::seconds(20));
    REQUIRE(gen.exit_code == 0);
    verify_crops(video_480p, "sd854_10bit", std::string("none"),
                 {SourceBox{111, 355, 501, 61}});
  }
}

TEST_CASE("FfmpegFrameSource max_output_width downscales 4K ROI and probes codec/pix_fmt",
          "[extractor][scale][integration]") {
  if (!available()) SKIP("ffmpeg/ffprobe not available");
  auto root_pattern = (std::filesystem::temp_directory_path() / "sublift_scale_XXXXXX").string();
  REQUIRE(::mkdtemp(root_pattern.data()) != nullptr);
  struct Fixture {
    std::filesystem::path root;
    ~Fixture() {
      std::error_code ec;
      std::filesystem::remove_all(root, ec);
    }
  } fixture{root_pattern};

  const std::string ffmpeg = resolve_ffmpeg_bin();
  const auto video = fixture.root / "uhd_yuv420p10le.nut";
  const auto gen = detail::run_subprocess(
      {ffmpeg, "-v", "error", "-y", "-f", "lavfi", "-i",
       "testsrc2=size=3840x2160:rate=5:duration=0.2", "-pix_fmt", "yuv420p10le",
       "-c:v", "rawvideo", video.string()},
      std::chrono::seconds(20));
  REQUIRE(gen.exit_code == 0);

  const SourceAndDuration probed = probe_source_and_duration(video);
  REQUIRE(probed.source.width == 3840);
  REQUIRE(probed.source.height == 2160);
  REQUIRE_FALSE(probed.codec_name.empty());
  REQUIRE(probed.pix_fmt == "yuv420p10le");

  const SourceBox uhd_roi{0, 1760, 3840, 300};
  FfmpegExtractorOptions scaled_opts{
      .enable_two_stage_crop = true,
      .max_output_width = 1920,
  };
  FfmpegFrameSource source(5.0, uhd_roi, probed.source, std::string("none"), scaled_opts);

  std::size_t frame_count = 0;
  source.extract(video, [&](Frame frame) {
    REQUIRE(frame.image.width() == 1920);
    REQUIRE(frame.image.height() == 150);
    REQUIRE(frame.image.format() == PixelFormat::RGB24);
    ++frame_count;
    return true;
  });
  REQUIRE(frame_count >= 1);
}

TEST_CASE("FfmpegFrameSource supports skip_frame auto at 60fps and explicit overrides",
          "[extractor][skip_frame][integration]") {
  if (!available()) SKIP("ffmpeg/ffprobe not available");
  auto root_pattern =
      (std::filesystem::temp_directory_path() / "sublift_skipframe_XXXXXX").string();
  REQUIRE(::mkdtemp(root_pattern.data()) != nullptr);
  struct Fixture {
    std::filesystem::path root;
    ~Fixture() {
      std::error_code ec;
      std::filesystem::remove_all(root, ec);
    }
  } fixture{root_pattern};

  const std::string ffmpeg = resolve_ffmpeg_bin();
  const auto video = fixture.root / "hd_60fps_bframes.mp4";
  const auto gen = detail::run_subprocess(
      {ffmpeg, "-v", "error", "-y", "-f", "lavfi", "-i",
       "testsrc2=size=320x180:rate=60:duration=0.6", "-pix_fmt", "yuv420p",
       "-c:v", "libx264", "-g", "30", "-bf", "4", video.string()},
      std::chrono::seconds(20));
  REQUIRE(gen.exit_code == 0);

  const SourceAndDuration probed = probe_source_and_duration(video);
  REQUIRE(probed.fps >= 59.0);

  const SourceBox crop{0, 120, 320, 48};
  for (const std::string& mode : {std::string("auto"), std::string("none"), std::string("noref")}) {
    FfmpegExtractorOptions opts;
    opts.skip_frame = mode;
    FfmpegFrameSource source(5.0, crop, std::nullopt, std::string("none"), opts);
    REQUIRE(resolve_effective_skip_frame(
                mode,
                StreamDecodeHints{.width = 320, .height = 180, .fps = probed.fps},
                5.0) == (mode == "none" ? "" : "noref"));
    std::vector<std::int64_t> timestamps;
    source.extract(video, [&](Frame frame) {
      REQUIRE(frame.image.width() == crop.width);
      REQUIRE(frame.image.height() == crop.height);
      REQUIRE(frame.image.format() == PixelFormat::RGB24);
      timestamps.push_back(frame.timestamp_ms);
      return true;
    });
    REQUIRE(timestamps.size() == 3);
    REQUIRE(timestamps[0] == 0);
    REQUIRE(timestamps[1] == 200);
    REQUIRE(timestamps[2] == 400);
  }

  FfmpegExtractorOptions bad_opts;
  bad_opts.skip_frame = "invalid";
  FfmpegFrameSource bad_source(5.0, crop, probed.source, std::string("none"), bad_opts);
  REQUIRE_THROWS_AS(bad_source.extract(video, [](Frame) { return true; }), std::invalid_argument);
}

TEST_CASE("FfmpegFrameSource sample_keyframe_thumbnails obeys max_samples and time range",
          "[extractor][keyframes][integration]") {
  if (!available()) SKIP("ffmpeg/ffprobe not available");
  auto root_pattern =
      (std::filesystem::temp_directory_path() / "sublift_keyframes_XXXXXX").string();
  REQUIRE(::mkdtemp(root_pattern.data()) != nullptr);
  struct Fixture {
    std::filesystem::path root;
    ~Fixture() {
      std::error_code ec;
      std::filesystem::remove_all(root, ec);
    }
  } fixture{root_pattern};

  const std::string ffmpeg = resolve_ffmpeg_bin();
  const auto video = fixture.root / "keyframes_4s.mp4";
  const auto gen = detail::run_subprocess(
      {ffmpeg, "-v", "error", "-y", "-f", "lavfi", "-i",
       "testsrc2=size=320x240:rate=25:duration=4", "-pix_fmt", "yuv420p",
       "-c:v", "libx264", "-g", "25", video.string()},
      std::chrono::seconds(20));
  REQUIRE(gen.exit_code == 0);

  FfmpegFrameSource source(5.0);
  const auto capped = source.sample_keyframe_thumbnails(video, 0.45, 2);
  REQUIRE(capped.size() >= 1);
  REQUIRE(capped.size() <= 2);
  for (const auto& thumb : capped) {
    REQUIRE_FALSE(thumb.image.empty());
    REQUIRE(thumb.image.width() == 320);
    REQUIRE(thumb.image.height() == 80);
    REQUIRE(thumb.image.format() == PixelFormat::Gray8);
  }

  source.set_time_range(1.5, 2.0);
  const auto ranged = source.sample_keyframe_thumbnails(video, 0.45, 8);
  REQUIRE(ranged.size() >= 1);
  for (const auto& thumb : ranged) {
    REQUIRE(thumb.timestamp_ms >= 1500);
    REQUIRE_FALSE(thumb.image.empty());
  }
}
