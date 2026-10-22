#include "sublift/server/region_detector.hpp"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

#include "engine_factory.hpp"
#include "sublift/adapters/ffmpeg.hpp"
#include "sublift/adapters/paddle.hpp"
#include "sublift/adapters/vision.hpp"
#include "sublift/subtitle_band.hpp"

namespace sublift::server {

RegionDetector::RegionDetector(std::shared_ptr<IOcrEngine> shared_ocr,
                               sublift::PaddleExecutionConfig paddle_execution)
    : shared_ocr_(std::move(shared_ocr)), paddle_execution_(paddle_execution) {}

IOcrEngine* RegionDetector::resolve_ocr_engine_locked(
    const std::optional<std::string>& engine_preference) {
  if (shared_ocr_ && !engine_preference.has_value()) {
    return shared_ocr_.get();
  }

  std::string target_engine = "mock";
  if (engine_preference.has_value() && !engine_preference->empty()) {
    target_engine = engine_preference.value();
  } else {
    if (sublift::is_vision_available()) {
      target_engine = "vision";
    } else if (sublift::is_paddle_available()) {
      target_engine = "paddle";
    } else {
      target_engine = "mock";
    }
  }

  if (fallback_engine_ && current_engine_name_ == target_engine) {
    return fallback_engine_.get();
  }

  sublift::worker::EngineFactory factory(target_engine, paddle_execution_);
  fallback_engine_ = factory.create_engine();
  current_engine_name_ = target_engine;
  return fallback_engine_.get();
}

RegionDetectionResult RegionDetector::detect_region(
    const std::filesystem::path& video_path,
    std::optional<double> playhead_time_s,
    const std::optional<std::string>& engine_preference) {
  std::error_code ec;
  if (!std::filesystem::exists(video_path, ec)) {
    throw std::runtime_error("视频文件不存在: " + video_path.string());
  }

  const VideoInfo info = sublift::ffmpeg::probe_video(video_path);
  const double duration_s = static_cast<double>(info.duration_ms) / 1000.0;
  const std::int32_t src_w = info.width;
  const std::int32_t src_h = info.height;

  if (src_w <= 0 || src_h <= 0) {
    RegionDetectionResult res;
    res.detected = false;
    res.sample_time_s = 0.0;
    res.suggested_box = NormalizedBox{0.0, 0.70, 1.0, 0.25};
    res.error_msg = "无法获取视频尺寸";
    return res;
  }

  const double requested_time_s =
      playhead_time_s.has_value()
          ? std::clamp(playhead_time_s.value(), 0.0,
                       std::max(0.0, duration_s))
          : 0.0;
  std::vector<double> timestamps;
  if (playhead_time_s.has_value()) {
    timestamps.push_back(requested_time_s);
  } else {
    if (duration_s <= 0.5) {
      timestamps.push_back(0.0);
    } else if (duration_s < 3.0) {
      timestamps.push_back(duration_s * 0.40);
      timestamps.push_back(duration_s * 0.70);
    } else if (duration_s < 8.0) {
      timestamps.push_back(duration_s * 0.20);
      timestamps.push_back(duration_s * 0.45);
      timestamps.push_back(duration_s * 0.70);
    } else {
      // 固定 7 个采样点；不再“单帧早停”，因为自适应条带需要跨帧支持度。
      const std::vector<double> ratios = {0.12, 0.25, 0.38, 0.50, 0.62, 0.75, 0.88};
      for (double r : ratios) {
        timestamps.push_back(duration_s * r);
      }
    }
  }

  std::vector<sublift::SubtitleBandLine> band_lines;
  std::vector<double> frame_times;
  frame_times.reserve(timestamps.size());

  for (double time_s : timestamps) {
    ImageBuffer frame;
    try {
      frame = sublift::ffmpeg::extract_single_frame_rgb24(video_path, time_s, 1920);
    } catch (const std::exception&) {
      continue;
    }
    if (frame.empty()) {
      continue;
    }

    OcrResult ocr_res;
    {
      std::lock_guard<std::mutex> lock(ocr_mutex_);
      IOcrEngine* engine = resolve_ocr_engine_locked(engine_preference);
      if (!engine) {
        throw std::runtime_error("无法初始化 OCR 引擎进行区域识别");
      }
      ocr_res = engine->recognize(frame.view());
    }

    const std::int32_t img_w = frame.width();
    const std::int32_t img_h = frame.height();
    if (img_w <= 0 || img_h <= 0) {
      continue;
    }

    const int frame_index = static_cast<int>(frame_times.size());
    frame_times.push_back(time_s);

    for (const auto& line : ocr_res.lines) {
      if (line.confidence < 0.40) {
        continue;
      }
      sublift::SubtitleBandLine band_line;
      band_line.frame_index = frame_index;
      band_line.x = static_cast<double>(line.box.x) / static_cast<double>(img_w);
      band_line.y = static_cast<double>(line.box.y) / static_cast<double>(img_h);
      band_line.width =
          static_cast<double>(line.box.width) / static_cast<double>(img_w);
      band_line.height =
          static_cast<double>(line.box.height) / static_cast<double>(img_h);
      band_line.text = line.text;
      band_line.confidence = line.confidence;
      band_lines.push_back(std::move(band_line));
    }
  }

  RegionDetectionResult result;
  if (frame_times.empty()) {
    result.detected = false;
    result.sample_time_s = requested_time_s;
    result.suggested_box = NormalizedBox{0.0, 0.70, 1.0, 0.25};
    result.error_msg = "无法抽取代表帧";
    return result;
  }

  sublift::SubtitleBandConfig band_config;
  band_config.sample_frames_hint = static_cast<int>(frame_times.size());
  const sublift::SubtitleBandResult band =
      sublift::detect_subtitle_band(band_lines, band_config);

  if (!band.detected) {
    result.detected = false;
    // 显式 playhead 重检失败时仍回填请求时间，便于 UI 标记“该帧未识别”。
    result.sample_time_s = requested_time_s;
    result.suggested_box = NormalizedBox{0.0, 0.70, 1.0, 0.25};
    result.preview_text = "";
    result.confidence = 0.0;
    result.total_candidates = 0;
    result.error_msg = band.diagnostics.reason.empty()
                           ? "no_subtitle_band"
                           : band.diagnostics.reason;
    return result;
  }

  const int anchor_frame = band.diagnostics.anchor_frame_index;
  if (anchor_frame >= 0 &&
      static_cast<std::size_t>(anchor_frame) < frame_times.size()) {
    result.sample_time_s = frame_times[static_cast<std::size_t>(anchor_frame)];
  } else {
    result.sample_time_s = frame_times.front();
  }
  result.detected = true;
  result.suggested_box = NormalizedBox{band.x, band.y, band.width, band.height};
  result.preview_text = band.preview_text;
  result.confidence = band.confidence;
  result.total_candidates = band.total_candidates;
  return result;
}

}  // namespace sublift::server
