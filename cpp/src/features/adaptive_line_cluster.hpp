#pragma once

#include <vector>

#include <opencv2/core.hpp>

#include "adaptive_component_filter.hpp"

namespace sublift::features::detail {

void cluster_polarity_text_lines(
    const std::vector<SubtitleComponent>& comps,
    const cv::Mat& labels,
    int num_labels,
    int width,
    int height,
    cv::Mat& mask_out,
    int& out_chars,
    int& out_area);

} // namespace sublift::features::detail
