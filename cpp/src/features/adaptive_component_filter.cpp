#include "adaptive_component_filter.hpp"

#include <algorithm>
#include <vector>

#include <opencv2/imgproc.hpp>

namespace sublift::features::detail {

namespace {

using cv::Mat;

bool has_two_sided_background_ring(
    const Mat& src,
    const Mat& gray,
    const Mat& labels,
    int label_idx,
    const cv::Rect& roi,
    const Mat& comp_mask,
    double mu_fg,
    double peak_c,
    bool is_pos_polarity) {
  const int w = src.cols;
  const int h = src.rows;
  const int pad = 5;
  const int rx = std::max(0, roi.x - pad);
  const int ry = std::max(0, roi.y - pad);
  const int rw = std::min(w - rx, roi.width + 2 * pad);
  const int rh = std::min(h - ry, roi.height + 2 * pad);
  const cv::Rect pad_roi(rx, ry, rw, rh);

  Mat pad_mask = (labels(pad_roi) == label_idx);

  // Fill internal holes so solid synthetic blocks evaluate outer background only
  Mat canvas = Mat::zeros(pad_mask.rows + 4, pad_mask.cols + 4, CV_8UC1);
  pad_mask.copyTo(canvas(cv::Rect(2, 2, pad_mask.cols, pad_mask.rows)));
  Mat flood_filled = canvas.clone();
  Mat flood_mask = Mat::zeros(canvas.rows + 2, canvas.cols + 2, CV_8UC1);
  cv::floodFill(flood_filled, flood_mask, cv::Point(0, 0), cv::Scalar(255));
  cv::bitwise_not(flood_filled, flood_filled);
  Mat filled_canvas = canvas | flood_filled;
  Mat filled_mask = filled_canvas(cv::Rect(2, 2, pad_mask.cols, pad_mask.rows));

  Mat pad_d7, pad_d3;
  const Mat k7 = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(7, 7));
  const Mat k3 = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 3));
  cv::dilate(filled_mask, pad_d7, k7);
  cv::dilate(filled_mask, pad_d3, k3);
  Mat ext_ring = pad_d7 & ~pad_d3;
  const int ring_total = cv::countNonZero(ext_ring);
  if (ring_total < 10) {
    return true;
  }

  const int bg_drop = std::max(20, static_cast<int>(0.40 * peak_c));
  Mat ext_bg;
  if (src.channels() == 3) {
    const cv::Scalar mean_color = cv::mean(src(roi), comp_mask);
    Mat pad_src = src(pad_roi);
    std::vector<Mat> channels(3);
    cv::split(pad_src, channels);
    Mat max_diff = Mat::zeros(pad_src.rows, pad_src.cols, CV_8UC1);
    for (int c = 0; c < 3; ++c) {
      Mat diff_c;
      if (is_pos_polarity) {
        cv::subtract(cv::Scalar(mean_color[c]), channels[c], diff_c);
      } else {
        cv::subtract(channels[c], cv::Scalar(mean_color[c]), diff_c);
      }
      cv::max(max_diff, diff_c, max_diff);
    }
    cv::bitwise_and(ext_ring, (max_diff >= bg_drop), ext_bg);
  } else {
    Mat pad_gray = gray(pad_roi);
    if (is_pos_polarity) {
      cv::bitwise_and(ext_ring, (pad_gray < static_cast<int>(mu_fg - bg_drop)), ext_bg);
    } else {
      cv::bitwise_and(ext_ring, (pad_gray > static_cast<int>(mu_fg + bg_drop)), ext_bg);
    }
  }
  const double bg_ratio = static_cast<double>(cv::countNonZero(ext_bg)) / ring_total;
  return bg_ratio >= 0.65;
}

} // namespace

PolarityContrastMaps compute_polarity_contrast_maps(
    const cv::Mat& src,
    const cv::Mat& gray,
    int block_size,
    double adaptive_c,
    double scale) {
  PolarityContrastMaps maps;
  Mat blur;
  if (src.channels() == 3) {
    Mat diff_pos, diff_neg;
    cv::GaussianBlur(src, blur, cv::Size(block_size, block_size), 0);
    cv::subtract(src, blur, diff_pos);
    cv::subtract(blur, src, diff_neg);
    std::vector<Mat> ch_pos(3), ch_neg(3);
    cv::split(diff_pos, ch_pos);
    cv::split(diff_neg, ch_neg);
    cv::max(ch_pos[0], ch_pos[1], maps.contrast_pos);
    cv::max(maps.contrast_pos, ch_pos[2], maps.contrast_pos);
    cv::max(ch_neg[0], ch_neg[1], maps.contrast_neg);
    cv::max(maps.contrast_neg, ch_neg[2], maps.contrast_neg);
  } else {
    cv::GaussianBlur(gray, blur, cv::Size(block_size, block_size), 0);
    cv::subtract(gray, blur, maps.contrast_pos);
    cv::subtract(blur, gray, maps.contrast_neg);
  }

  const double th_low = std::min(18.0, adaptive_c);
  cv::threshold(maps.contrast_pos, maps.cand_pos, th_low, 255, cv::THRESH_BINARY);
  cv::threshold(maps.contrast_neg, maps.cand_neg, th_low, 255, cv::THRESH_BINARY);
  if (scale >= 0.6) {
    const Mat kernel2 = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(2, 2));
    cv::morphologyEx(maps.cand_pos, maps.cand_pos, cv::MORPH_OPEN, kernel2);
    cv::morphologyEx(maps.cand_neg, maps.cand_neg, cv::MORPH_OPEN, kernel2);
  }

  const int blur_rad = (block_size - 1) / 2;
  const int k_rad = std::max(blur_rad, std::max(2, static_cast<int>(3 * scale)));
  const Mat k_outline =
      cv::getStructuringElement(cv::MORPH_RECT, cv::Size(2 * k_rad + 1, 2 * k_rad + 1));
  Mat dark_outline, bright_outline;
  cv::bitwise_and(maps.cand_neg, (gray <= 80), dark_outline);
  cv::bitwise_and(maps.cand_pos, (gray >= 180), bright_outline);
  cv::dilate(dark_outline, maps.dilated_dark_outline, k_outline);
  cv::dilate(bright_outline, maps.dilated_bright_outline, k_outline);
  return maps;
}

std::vector<SubtitleComponent> filter_polarity_components(
    const cv::Mat& src,
    const cv::Mat& gray,
    const cv::Mat& cand_mask,
    const cv::Mat& other_dilated,
    const cv::Mat& c_map,
    bool is_pos_polarity,
    cv::Mat& labels_out,
    int& num_labels_out) {
  Mat stats, centroids;
  num_labels_out = cv::connectedComponentsWithStats(
      cand_mask, labels_out, stats, centroids, 8, CV_32S);
  if (num_labels_out <= 1) {
    return {};
  }

  const int h = src.rows;
  const int w = src.cols;
  const double scale = h / 120.0;
  const int min_h = std::max(8, static_cast<int>(10 * scale));
  const int max_h = static_cast<int>(0.85 * h);
  const int min_area = std::max(12, static_cast<int>(20 * scale * scale));
  const int max_single_comp_w = static_cast<int>(0.88 * w);
  const int corridor_x_min = static_cast<int>(0.18 * w);
  const int corridor_x_max = static_cast<int>(0.82 * w);
  const int r_erode = std::max(2, static_cast<int>(5 * scale));
  const Mat k_erode =
      cv::getStructuringElement(cv::MORPH_RECT, cv::Size(2 * r_erode + 1, 2 * r_erode + 1));

  std::vector<SubtitleComponent> qual_comps;
  qual_comps.reserve(static_cast<std::size_t>(num_labels_out));

  for (int i = 1; i < num_labels_out; ++i) {
    const int cw = stats.at<int>(i, cv::CC_STAT_WIDTH);
    const int ch = stats.at<int>(i, cv::CC_STAT_HEIGHT);
    const int cx = stats.at<int>(i, cv::CC_STAT_LEFT);
    const int cy = stats.at<int>(i, cv::CC_STAT_TOP);
    const int area = stats.at<int>(i, cv::CC_STAT_AREA);

    const bool is_h_stroke =
        (ch >= std::max(3, static_cast<int>(4 * scale)) &&
         cw >= std::max(24, static_cast<int>(35 * scale)) &&
         cw <= static_cast<int>(0.65 * h));
    if (!is_h_stroke && ch < min_h) continue;
    if (ch > max_h) continue;
    if (!is_h_stroke && area < min_area) continue;
    if (cw < 3 || cw > max_single_comp_w) continue;
    if (cy <= 1 || cy + ch >= h - 1) continue;
    if (ch >= static_cast<int>(0.80 * h)) continue;

    const cv::Rect roi(cx, cy, cw, ch);
    Mat comp_mask = (labels_out(roi) == i);

    double min_val = 0.0, peak_c = 0.0;
    cv::minMaxLoc(c_map(roi), &min_val, &peak_c, nullptr, nullptr, comp_mask);

    double min_g = 0.0, max_g = 0.0;
    cv::minMaxLoc(gray(roi), &min_g, &max_g, nullptr, nullptr, comp_mask);

    Mat overlap;
    cv::bitwise_and(comp_mask, other_dilated(roi), overlap);
    const double coupling_ratio =
        static_cast<double>(cv::countNonZero(overlap)) / std::max(1, area);
    const bool is_coupled =
        (coupling_ratio >= 0.25 && peak_c >= 30.0 &&
         (is_pos_polarity ? (max_g >= 175.0) : (min_g <= 75.0)));

    if (ch < min_h && (!is_coupled || peak_c < 80.0)) continue;

    const double mu_fg = cv::mean(gray(roi), comp_mask)[0];
    if (!has_two_sided_background_ring(src, gray, labels_out, i, roi, comp_mask,
                                       mu_fg, peak_c, is_pos_polarity)) {
      continue;
    }

    if (!is_coupled && peak_c < 40.0) continue;
    if (ch < min_h && (!is_coupled || peak_c < 70.0)) continue;

    const int center_x = cx + cw / 2;
    const bool in_corridor = (center_x >= corridor_x_min && center_x <= corridor_x_max);

    Mat eroded;
    cv::erode(comp_mask, eroded, k_erode);
    const double solid_core_ratio =
        static_cast<double>(cv::countNonZero(eroded)) / std::max(1, area);
    if (!is_coupled && solid_core_ratio > 0.25) {
      const bool is_centered_bar = (in_corridor && ch >= min_h &&
                                    ch <= static_cast<int>(0.55 * h) &&
                                    cw <= max_single_comp_w);
      if (!is_centered_bar) continue;
    }
    if (solid_core_ratio > 0.50 && ch >= static_cast<int>(0.65 * h)) continue;

    qual_comps.push_back(SubtitleComponent{
        i, cx, cy, cw, ch, area, in_corridor, cy + ch, peak_c, is_coupled});
  }
  return qual_comps;
}

} // namespace sublift::features::detail
