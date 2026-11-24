#include <sublift/engine/auto_region.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

#include <sublift/core/image_view.hpp>
#include <sublift/text/region_cluster.hpp>

namespace sublift {

namespace {

struct ThumbnailEnergy {
  std::int64_t timestamp_ms{0};
  int energy{0};
  int peak_row{0};
  int upper_mean{0};
  std::array<int, 80> row_sums{};
};

ThumbnailEnergy evaluate_thumbnail_energy(const ImageView& view, std::int64_t ts_ms) {
  ThumbnailEnergy res;
  res.timestamp_ms = ts_ms;
  if (view.empty() || view.width() != 320 || view.height() != 80) {
    return res;
  }

  for (int y = 1; y < 79; ++y) {
    const auto* p_prev = view.row(y - 1);
    const auto* p_curr = view.row(y);
    const auto* p_next = view.row(y + 1);
    int count = 0;
    for (int x = 81; x < 239; ++x) {
      const int dx = (static_cast<int>(p_prev[x + 1]) + 2 * static_cast<int>(p_curr[x + 1]) + static_cast<int>(p_next[x + 1])) -
                     (static_cast<int>(p_prev[x - 1]) + 2 * static_cast<int>(p_curr[x - 1]) + static_cast<int>(p_next[x - 1]));
      if (std::abs(dx) / 4 > 35) {
        ++count;
      }
    }
    res.row_sums[y] = count;
  }

  constexpr int kWin = 12;
  int max_sub_band = 0;
  int peak_y = 24;
  for (int y = 24; y <= 68; ++y) {
    int sum = 0;
    for (int w = 0; w < kWin && (y + w) < 80; ++w) {
      sum += res.row_sums[y + w];
    }
    if (sum > max_sub_band) {
      max_sub_band = sum;
      peak_y = y;
    }
  }

  int upper_sum_12 = 0;
  for (int y = 0; y < 20; ++y) {
    int win_sum = 0;
    for (int w = 0; w < kWin && (y + w) < 80; ++w) {
      win_sum += res.row_sums[y + w];
    }
    upper_sum_12 += win_sum;
  }

  res.energy = max_sub_band;
  res.peak_row = peak_y;
  res.upper_mean = upper_sum_12 / 20;
  return res;
}

} // namespace

AutoRegionResult detect_auto_region(
    IFrameSource& source,
    IOcrEngine& ocr,
    const std::filesystem::path& video_path,
    const AutoRegionOptions& options) {
  AutoRegionResult result;
  const VideoInfo info = source.probe(video_path);
  if (info.width <= 0 || info.height <= 0) {
    result.error_message = "无法探测视频有效分辨率";
    return result;
  }

  // 1. Stage 1: 获取关键帧缩略图流并执行 Sobel 边缘能量初筛
  std::vector<std::int64_t> candidate_timestamps;
  std::vector<ThumbnailEnergy> energies;
  const double safe_bottom_ratio =
      (std::isnan(options.bottom_ratio) || options.bottom_ratio <= 0.0 ||
       options.bottom_ratio > 1.0)
          ? 0.45
          : options.bottom_ratio;
  auto thumbnails =
      source.sample_keyframe_thumbnails(video_path, safe_bottom_ratio, 240);
  if (!thumbnails.empty()) {
    energies.reserve(thumbnails.size());
    for (const auto& thumb : thumbnails) {
      energies.push_back(evaluate_thumbnail_energy(thumb.image.view(), thumb.timestamp_ms));
    }
    const auto is_prominent = [](const ThumbnailEnergy& e) noexcept {
      return e.energy >= 140 &&
             static_cast<double>(e.energy) >= static_cast<double>(e.upper_mean) * 1.5;
    };
    std::sort(energies.begin(), energies.end(), [&](const auto& a, const auto& b) {
      const bool pa = is_prominent(a);
      const bool pb = is_prominent(b);
      if (pa != pb) {
        return pa > pb;
      }
      return a.energy > b.energy;
    });

    for (const auto& e : energies) {
      if (candidate_timestamps.size() >= options.max_ocr_candidates) {
        break;
      }
      if (e.energy < 100) {
        continue;
      }
      bool spaced = true;
      for (auto chosen_ts : candidate_timestamps) {
        if (std::abs(chosen_ts - e.timestamp_ms) < 4000) {
          spaced = false;
          break;
        }
      }
      if (spaced) {
        candidate_timestamps.push_back(e.timestamp_ms);
      }
    }
  }

  // 立即释放关键帧缩略图像素缓冲，消除前置阶段内存常驻
  thumbnails.clear();
  thumbnails.shrink_to_fit();

  // 备用兜底：若缩略图为空或未筛选出足够能量候选，均匀采样 4 个时间跨度点
  if (candidate_timestamps.empty()) {
    const std::int64_t dur = info.duration_ms > 0 ? info.duration_ms : 60000;
    candidate_timestamps = {dur / 5, 2 * dur / 5, 3 * dur / 5, 4 * dur / 5};
  }

  // 2. Stage 2: 原始帧抽取 + 下半屏 ROI OCR + 纯算法抗噪聚类
  std::vector<text::FrameClusterCandidate> valid_clusters;
  const text::LineClusterOptions cluster_opts{
      .target_script = options.script,
      .min_confidence = options.min_ocr_confidence,
  };

  std::int64_t best_ts = 0;
  bool has_multiline_cluster = false;
  std::size_t ocr_calls = 0;
  std::vector<std::int64_t> probed_timestamps;

  const auto probe_timestamp = [&](std::int64_t ts) -> bool {
    probed_timestamps.push_back(ts);
    ++ocr_calls;

    auto frame = source.extract_frame(video_path, ts, std::nullopt);
    if (frame.empty()) {
      return false;
    }

    const auto frame_w = frame.width();
    const auto frame_h = frame.height();
    const std::int32_t y_offset = (frame_h >= 64) ? (frame_h / 2) : 0;

    ImageView ocr_view = (y_offset > 0 && frame_h > y_offset)
        ? frame.view().roi(0, y_offset, frame_w, frame_h - y_offset)
        : frame.view();

    auto ocr_res = ocr.recognize(ocr_view);
    if (y_offset > 0) {
      for (auto& line : ocr_res.lines) {
        line.box.y += y_offset;
      }
    }

    auto cluster = text::cluster_frame_lines(
        ocr_res.lines, frame_w, frame_h, info.width, info.height, cluster_opts);
    if (cluster.detected && cluster.cluster_score > 0.0) {
      if (valid_clusters.empty()) {
        best_ts = ts;
      }
      if (cluster.total_candidates >= 2) {
        has_multiline_cluster = true;
        best_ts = ts;
      }
      valid_clusters.push_back(std::move(cluster));
      return true;
    }
    return false;
  };

  for (std::int64_t ts : candidate_timestamps) {
    if (probe_timestamp(ts) && valid_clusters.size() >= options.min_ocr_candidates) {
      break; // 收集到足够高质量候选帧，快速收敛
    }
  }

  // 3. 多候选帧合并与未检出平滑回退
  const auto make_fallback = [&](std::string err_msg) {
    if (options.fallback_to_default) {
      const double ratio = (std::isnan(options.bottom_ratio) ||
                            options.bottom_ratio <= 0.0 ||
                            options.bottom_ratio > 1.0)
                               ? 0.45
                               : options.bottom_ratio;
      const std::int32_t fallback_h = std::clamp(
          static_cast<std::int32_t>(std::round(info.height * ratio)), 1, info.height);
      const std::int32_t fallback_y = info.height - fallback_h;
      result.detected = false;
      result.region_box = SourceBox{0, fallback_y, info.width, fallback_h};
      result.profile = SubtitleProfile::from_crop(info.width, fallback_h, options.script);
      result.error_message = err_msg + "，平滑回退默认底部比例";
    } else {
      result.detected = false;
      result.error_message = std::move(err_msg);
    }
    return result;
  };

  if (valid_clusters.empty()) {
    return make_fallback("未检出有效字幕行");
  }

  auto merged = text::merge_cluster_candidates(valid_clusters, info.width, info.height);
  if (!merged.detected || merged.selection.width <= 0 || merged.selection.height <= 0) {
    return make_fallback("多候选帧合并未检出有效区域");
  }

  // Stage 2b: 若 Stage 2a 仅捕获单行字幕，基于首轮位置回扫缩略图双峰分布以验证是否存在双行上行字幕
  if (!has_multiline_cluster && !energies.empty() && ocr_calls < options.max_ocr_candidates) {
    const int crop_h_px =
        std::max(2, (static_cast<int>(info.height * safe_bottom_ratio) / 2) * 2);
    const int crop_y_px = std::max(0, info.height - crop_h_px);
    const double scale_to_thumb = 80.0 / static_cast<double>(crop_h_px);
    const double init_raw_h =
        (std::isnan(merged.h_char_px) || merged.h_char_px < 1.0) ? 1.0 : merged.h_char_px;
    const double cy_video =
        static_cast<double>(merged.selection.y) +
        static_cast<double>(merged.selection.height) * 0.5;
    const double cy_thumb = (cy_video - static_cast<double>(crop_y_px)) * scale_to_thumb;
    const double est_glyph_h_px = std::clamp(
        init_raw_h * 0.85,
        static_cast<double>(info.height) * 0.036,
        static_cast<double>(info.height) * 0.056);
    const int h_rows = std::clamp(
        static_cast<int>(std::lround(est_glyph_h_px * scale_to_thumb)), 6, 12);
    const int r_lo0 = std::clamp(
        static_cast<int>(std::lround(cy_thumb - static_cast<double>(h_rows) * 0.45)),
        8, 72);
    const int r_lo1 = std::min(78, r_lo0 + h_rows);
    const int r_up1 = std::max(4, r_lo0 - 1);
    const int r_up0 = std::max(2, r_lo0 - h_rows - 1);

    if (r_up1 > r_up0 && r_lo1 > r_lo0) {
      struct BimodalCand {
        double score{0.0};
        std::int64_t ts{0};
      };
      std::vector<BimodalCand> bimodal_cands;
      for (const auto& e : energies) {
        if (std::find(probed_timestamps.begin(), probed_timestamps.end(), e.timestamp_ms) !=
            probed_timestamps.end()) {
          continue;
        }
        double up_sum = 0.0;
        for (int r = r_up0; r < r_up1; ++r) {
          up_sum += e.row_sums[r];
        }
        const double up_mean = up_sum / static_cast<double>(r_up1 - r_up0);

        double lo_sum = 0.0;
        for (int r = r_lo0; r < r_lo1; ++r) {
          lo_sum += e.row_sums[r];
        }
        const double lo_mean = lo_sum / static_cast<double>(r_lo1 - r_lo0);

        const int q0 = std::max(2, r_up0 - 6);
        const int q1 = std::max(q0 + 1, r_up0 - 1);
        double q_sum = 0.0;
        for (int r = q0; r < q1; ++r) {
          q_sum += e.row_sums[r];
        }
        const double quiet_mean = q_sum / static_cast<double>(q1 - q0);

        const int v0 = std::max(1, r_up1 - 1);
        const int v1 = std::min(79, r_lo0 + 2);
        int valley = e.row_sums[v0];
        for (int r = v0 + 1; r < v1; ++r) {
          valley = std::min(valley, e.row_sums[r]);
        }

        const double up_excess = up_mean - quiet_mean * 1.25;
        const double lo_excess = lo_mean - quiet_mean * 1.15;
        const double min_band = std::min(up_mean, lo_mean);
        const double max_band = std::max(up_mean, lo_mean);
        if (up_excess > 2.0 && lo_excess > 1.8 && min_band >= 6.0 &&
            min_band >= 0.25 * max_band &&
            static_cast<double>(valley) <= 0.72 * min_band) {
          const double sc = std::min(up_excess, lo_excess) *
                            (1.0 - static_cast<double>(valley) / std::max(1.0, min_band));
          bimodal_cands.push_back(BimodalCand{.score = sc, .ts = e.timestamp_ms});
        }
      }

      std::sort(bimodal_cands.begin(), bimodal_cands.end(),
                [](const auto& a, const auto& b) { return a.score > b.score; });

      std::size_t bimodal_probed = 0;
      std::vector<std::int64_t> stage2b_chosen;
      for (const auto& bc : bimodal_cands) {
        if (bimodal_probed >= 2 || ocr_calls >= options.max_ocr_candidates) {
          break;
        }
        bool spaced = true;
        for (auto prev_ts : stage2b_chosen) {
          if (std::abs(prev_ts - bc.ts) < 1000) {
            spaced = false;
            break;
          }
        }
        if (!spaced) {
          continue;
        }
        stage2b_chosen.push_back(bc.ts);
        ++bimodal_probed;
        probe_timestamp(bc.ts);
        if (has_multiline_cluster) {
          break; // Verified 2-line subtitle envelope; stop immediately
        }
      }
      if (bimodal_probed > 0) {
        merged = text::merge_cluster_candidates(valid_clusters, info.width, info.height);
        if (!merged.detected || merged.selection.width <= 0 || merged.selection.height <= 0) {
          return make_fallback("多候选帧合并未检出有效区域");
        }
      }
    }
  }

  // 4. 施加适度安全 padding 与 4.5% 最小高度保底
  const double raw_h = (std::isnan(merged.h_char_px) || merged.h_char_px < 1.0) ? 1.0 : merged.h_char_px;
  const double pad = std::clamp(raw_h * 0.08, 4.0, 10.0);

  std::int32_t y0 = std::clamp(
      static_cast<std::int32_t>(std::round(merged.selection.y - pad)), 0, info.height);
  std::int32_t y1 = std::clamp(
      static_cast<std::int32_t>(std::round(merged.selection.y + merged.selection.height + pad)), y0, info.height);

  // 对齐到偶数边界（FFmpeg 与很多硬件解码器对偶数高度更友好）
  if (y0 % 2 != 0) {
    y0 = std::max(0, y0 - 1);
  }
  if (y1 % 2 != 0) {
    y1 = std::min(info.height, y1 + 1);
  }

  const std::int32_t min_h = std::clamp(
      static_cast<std::int32_t>(std::round(info.height * 0.045)), 1, info.height);
  if (y1 - y0 < min_h) {
    const std::int32_t diff = min_h - (y1 - y0);
    y0 = std::max(0, y0 - diff / 2);
    y1 = std::min(info.height, y0 + min_h);
    if (y1 - y0 < min_h && info.height >= min_h) {
      y0 = info.height - min_h;
      y1 = info.height;
    }
  }

  y0 = std::clamp(y0, 0, info.height);
  y1 = std::clamp(y1, y0, info.height);

  const SourceBox region_box{0, y0, info.width, y1 - y0};

  // 5. 坐标系相对映射与脚本透传 (根治缺陷 1 与缺陷 2)
  result.detected = true;
  result.region_box = region_box;
  result.profile = SubtitleProfile::from_selection_in_video(region_box, merged.selection, options.script);
  if (has_multiline_cluster &&
      static_cast<double>(merged.selection.height) > raw_h * 1.25) {
    result.profile.height = std::clamp<std::int32_t>(
        static_cast<std::int32_t>(std::lround(raw_h)),
        1,
        std::max<std::int32_t>(1, result.profile.height));
  }
  result.sample_timestamp_ms = best_ts;
  result.preview_text = std::move(merged.preview_text);
  result.confidence = std::isnan(merged.confidence) ? 0.0 : std::clamp(merged.confidence, 0.0, 1.0);
  return result;
}

} // namespace sublift
