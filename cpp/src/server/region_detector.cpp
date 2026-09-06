#include "sublift/server/region_detector.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <unordered_set>

#include "engine_factory.hpp"
#include "sublift/adapters/ffmpeg.hpp"
#include "sublift/adapters/paddle.hpp"
#include "sublift/adapters/vision.hpp"

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

  std::vector<double> timestamps;
  if (playhead_time_s.has_value()) {
    double t = std::clamp(playhead_time_s.value(), 0.0, std::max(0.0, duration_s));
    timestamps.push_back(t);
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
      const std::vector<double> ratios = {0.12, 0.25, 0.38, 0.50, 0.62, 0.75, 0.88};
      std::unordered_set<std::int64_t> seen_ms;
      for (double r : ratios) {
        double sec = duration_s * r;
        auto ms = static_cast<std::int64_t>(sec * 10.0);
        if (seen_ms.insert(ms).second) {
          timestamps.push_back(sec);
        }
      }
    }
  }

  double best_score = -1.0;
  double best_time_s = 0.0;
  std::vector<OcrLine> best_candidates;
  std::int32_t detected_img_w = src_w;
  std::int32_t detected_img_h = src_h;

  for (double time_s : timestamps) {
    ImageBuffer frame;
    try {
      frame = sublift::ffmpeg::extract_single_frame_rgb24(video_path, time_s, 1920);
    } catch (const std::exception&) {
      continue;
    }

    if (frame.empty()) continue;

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
    if (img_w <= 0 || img_h <= 0) continue;

    double current_score = 0.0;
    std::vector<OcrLine> valid_lines;

    for (const auto& line : ocr_res.lines) {
      if (line.confidence < 0.40) continue;

      const double norm_y = static_cast<double>(line.box.y) / static_cast<double>(img_h);
      const double norm_x = static_cast<double>(line.box.x) / static_cast<double>(img_w);
      const double norm_w = static_cast<double>(line.box.width) / static_cast<double>(img_w);
      const double norm_xc = norm_x + norm_w / 2.0;

      // 仅关注下半区（y in [0.55, 0.98]）的字幕文本
      if (norm_y >= 0.55 && norm_y <= 0.98) {
        double weight = line.confidence;

        // 居中度奖励：中心距离中轴 25% 宽度以内
        if (std::abs(norm_xc - 0.5) < 0.25) {
          weight *= 1.5;
        }

        // 适中宽度奖励：排除单字噪点
        if (norm_w >= 0.15) {
          weight *= 1.3;
        }

        // 角落台标/小水印惩罚
        if ((norm_x > 0.75 || norm_x + norm_w < 0.25) && norm_w < 0.20) {
          weight *= 0.3;
        }

        current_score += weight;
        valid_lines.push_back(line);
      }
    }

    if (current_score > best_score) {
      best_score = current_score;
      best_time_s = time_s;
      best_candidates = std::move(valid_lines);
      detected_img_w = img_w;
      detected_img_h = img_h;
    }

    // 早停条件：单帧高置信度字幕 (Score >= 1.5)
    if (best_score >= 1.5) {
      break;
    }
  }

  RegionDetectionResult result;
  result.sample_time_s = best_time_s;

  if (!best_candidates.empty()) {
    std::int32_t min_y = best_candidates[0].box.y;
    std::int32_t max_y = best_candidates[0].box.y + best_candidates[0].box.height;
    double total_conf = 0.0;
    std::ostringstream ss_preview;

    for (const auto& line : best_candidates) {
      min_y = std::min(min_y, static_cast<std::int32_t>(line.box.y));
      max_y = std::max(max_y, static_cast<std::int32_t>(line.box.y + line.box.height));
      total_conf += line.confidence;
      if (ss_preview.tellp() > 0) ss_preview << " ";
      ss_preview << line.text;
    }

    const double pad = std::max(8.0, static_cast<double>(detected_img_h) * 0.012);
    const double y_pixel_min = std::max(0.0, static_cast<double>(min_y) - pad);
    const double y_pixel_max = std::min(static_cast<double>(detected_img_h), static_cast<double>(max_y) + pad);

    double box_y = y_pixel_min / static_cast<double>(detected_img_h);
    double box_h = (y_pixel_max - y_pixel_min) / static_cast<double>(detected_img_h);

    // 施加保底最小高度（4.5%），单行紧凑、多行自适应
    if (box_h < 0.045) {
      double diff = 0.045 - box_h;
      box_y = std::max(0.0, box_y - diff / 2.0);
      box_h = std::min(1.0 - box_y, 0.045);
    }

    box_y = std::clamp(box_y, 0.0, 0.95);
    box_h = std::clamp(box_h, 0.03, 1.0 - box_y);

    result.detected = true;
    result.suggested_box = NormalizedBox{0.0, box_y, 1.0, box_h};
    result.preview_text = ss_preview.str();
    result.confidence = total_conf / static_cast<double>(best_candidates.size());
    result.total_candidates = static_cast<int>(best_candidates.size());
  } else {
    result.detected = false;
    result.suggested_box = NormalizedBox{0.0, 0.70, 1.0, 0.25};
    result.preview_text = "";
    result.confidence = 0.0;
    result.total_candidates = 0;
  }

  return result;
}

}  // namespace sublift::server
