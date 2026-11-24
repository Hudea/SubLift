#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>

#include <sublift/adapters/ffmpeg/ffmpeg_frame_source.hpp>
#include <sublift/adapters/ffmpeg/fixed_detector.hpp>
#include <sublift/adapters/mock/mock_subtitle_sink.hpp>
#include <sublift/adapters/paddle/paddle_ocr.hpp>
#include <sublift/core/box.hpp>
#include <sublift/engine/config.hpp>
#include <sublift/engine/session.hpp>
#include <sublift/text/srt.hpp>

#include "process_utils.hpp"

#ifndef SUBLIFT_PROJECT_SOURCE_DIR
#define SUBLIFT_PROJECT_SOURCE_DIR ""
#endif

#ifndef SUBLIFT_PYTHON_EXECUTABLE
#define SUBLIFT_PYTHON_EXECUTABLE "python3"
#endif

namespace {

std::filesystem::path repo_root() {
  if (std::string(SUBLIFT_PROJECT_SOURCE_DIR).empty()) {
    return std::filesystem::current_path();
  }
  return std::filesystem::path{SUBLIFT_PROJECT_SOURCE_DIR};
}

}  // namespace

TEST_CASE("Real video short-clip E2E smoke extraction", "[e2e][smoke]") {
  if (!sublift::adapters::ffmpeg::available()) {
    SKIP("FFmpeg or ffprobe executable not available on host");
  }
  if (!sublift::adapters::paddle::is_paddle_available()) {
    SKIP("PaddleOCR model bundle not installed on host");
  }

  const auto root = repo_root();
  const auto gates_path = root / "benchmark/gates.json";
  REQUIRE(std::filesystem::exists(gates_path));

  std::ifstream gates_in(gates_path);
  REQUIRE(gates_in.is_open());
  const nlohmann::json gates = nlohmann::json::parse(gates_in);

  const auto& dataset = gates.at("dataset");
  const auto& clip = gates.at("e2e_smoke_clip");

  const std::filesystem::path video_path = root / dataset.at("video").get<std::string>();
  const std::filesystem::path ref_srt_path = root / dataset.at("reference_srt").get<std::string>();
  if (!std::filesystem::exists(video_path) || !std::filesystem::exists(ref_srt_path)) {
    SKIP("Zootopia smoke test assets not found under repo debug/smoke-test-data");
  }

  const double fps = dataset.at("fps").get<double>();
  const double start_sec = clip.at("start_sec").get<double>();
  const double duration_sec = clip.at("duration_sec").get<double>();
  const auto& tr = clip.at("time_range_ms");
  const std::string time_range_arg =
      std::to_string(tr.at(0).get<long long>()) + ":" +
      std::to_string(tr.at(1).get<long long>());
  const auto& reg = clip.at("region");
  const sublift::SourceBox region_box{
      reg.at(0).get<std::int32_t>(),
      reg.at(1).get<std::int32_t>(),
      reg.at(2).get<std::int32_t>(),
      reg.at(3).get<std::int32_t>(),
  };

  sublift::adapters::ffmpeg::FfmpegFrameSource source(fps);
  source.set_time_range(start_sec, duration_sec);

  const sublift::VideoInfo video_info = source.probe(video_path);
  REQUIRE(video_info.width == 1920);
  REQUIRE(video_info.height == 1080);

  sublift::adapters::paddle::PaddleOptions paddle_opts;
  paddle_opts.model_type = "small";
  paddle_opts.intra_op_threads = 4;
  paddle_opts.det_geometry.pixel_budget = 600000.0;
  paddle_opts.det_geometry.reference_width =
      std::min(2000.0, static_cast<double>(video_info.width));
  sublift::adapters::paddle::PaddleOcrEngine ocr(paddle_opts);

  sublift::adapters::ffmpeg::FixedRegionDetector detector(sublift::FrameLocalBox{
      region_box.x, region_box.y, region_box.width, region_box.height});
  sublift::adapters::mock::MockSubtitleSink sink;

  sublift::ExtractionConfig cfg;
  cfg.sample_fps = fps;
  cfg.confidence_threshold = 0.0;
  cfg.subtitle_script = "cjk";
  cfg.drop_empty_text = false;

  sublift::ExtractionSession session(source, ocr, &detector, &sink, cfg);
  const auto entries = session.process_video(video_path, region_box);

  const std::size_t cues_min = clip.at("cues_min").get<std::size_t>();
  const std::size_t cues_max = clip.at("cues_max").get<std::size_t>();
  INFO("Extracted cue count: " << entries.size());
  REQUIRE(entries.size() >= cues_min);
  REQUIRE(entries.size() <= cues_max);

  for (const auto& req_sub : clip.at("required_substrings")) {
    const std::string needle = req_sub.get<std::string>();
    const bool found = std::any_of(entries.begin(), entries.end(), [&](const auto& e) {
      return e.text.find(needle) != std::string::npos;
    });
    INFO("Expected substring: " << needle);
    CHECK(found);
  }

  std::filesystem::create_directories(root / "build");
  const auto temp_srt = (root / "build") /
      ("sublift_e2e_smoke_" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
       ".srt");
  struct Guard {
    std::filesystem::path p;
    ~Guard() {
      std::error_code ec;
      std::filesystem::remove(p, ec);
    }
  } guard{temp_srt};

  {
    std::ofstream out(temp_srt, std::ios::binary | std::ios::trunc);
    REQUIRE(out.is_open());
    out << sublift::text::format_srt(entries);
  }

  const std::string python_exec =
      std::string(SUBLIFT_PYTHON_EXECUTABLE).empty()
          ? "python3"
          : std::string(SUBLIFT_PYTHON_EXECUTABLE);
  const auto eval_res = sublift::adapters::ffmpeg::detail::run_subprocess(
      {python_exec,
       (root / "scripts/srt_eval.py").string(),
       ref_srt_path.string(),
       temp_srt.string(),
       "--time-range-ms",
       time_range_arg,
       "--json"},
      std::chrono::seconds(30));

  INFO("srt_eval stderr: " << eval_res.stderr_str);
  INFO("srt_eval stdout: " << eval_res.stdout_str);
  REQUIRE(eval_res.exit_code == 0);

  const nlohmann::json eval_payload = nlohmann::json::parse(eval_res.stdout_str);
  const auto& eval = eval_payload.at("eval");
  const double f1 = eval.at("f1").get<double>();
  const double cer = eval.at("cer_matched").get<double>();

  CHECK(f1 >= clip.at("f1_min").get<double>());
  CHECK(cer <= clip.at("cer_matched_max").get<double>());
}
