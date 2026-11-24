#pragma once

#include <vector>

#include <opencv2/core.hpp>

namespace sublift::features::detail {

struct SubtitleComponent {
  int label{0};
  int x{0};
  int y{0};
  int w{0};
  int h{0};
  int area{0};
  bool in_corridor{false};
  int y_bot{0};
  double peak_c{0.0};
  bool is_coupled{false};
};

struct PolarityContrastMaps {
  cv::Mat cand_pos;
  cv::Mat cand_neg;
  cv::Mat contrast_pos;
  cv::Mat contrast_neg;
  cv::Mat dilated_dark_outline;
  cv::Mat dilated_bright_outline;
};

[[nodiscard]] PolarityContrastMaps compute_polarity_contrast_maps(
    const cv::Mat& src,
    const cv::Mat& gray,
    int block_size,
    double adaptive_c,
    double scale);

[[nodiscard]] std::vector<SubtitleComponent> filter_polarity_components(
    const cv::Mat& src,
    const cv::Mat& gray,
    const cv::Mat& cand_mask,
    const cv::Mat& other_dilated,
    const cv::Mat& c_map,
    bool is_pos_polarity,
    cv::Mat& labels_out,
    int& num_labels_out);

} // namespace sublift::features::detail
