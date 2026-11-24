#include "adaptive_presence_mask.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <stdexcept>

#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <sublift/features/dhash.hpp>

#include "adaptive_component_filter.hpp"
#include "adaptive_line_cluster.hpp"
#include "opencv_thread_guard.hpp"

namespace sublift::features {

namespace {

using cv::Mat;

Mat to_gray_mat(const ImageView& view) {
  if (view.empty()) {
    throw std::invalid_argument("to_gray: empty image");
  }
  const auto step = static_cast<std::size_t>(view.stride_bytes());
  if (view.format() == PixelFormat::Gray8) {
    return Mat(view.height(), view.width(), CV_8UC1,
               const_cast<std::uint8_t*>(view.data()), step);
  }
  Mat src(view.height(), view.width(), CV_8UC3,
          const_cast<std::uint8_t*>(view.data()), step);
  Mat gray;
  cv::cvtColor(src, gray, cv::COLOR_RGB2GRAY);
  return gray;
}

Mat to_src_mat(const ImageView& view) {
  if (view.empty()) {
    throw std::invalid_argument("to_src_mat: empty image");
  }
  const auto step = static_cast<std::size_t>(view.stride_bytes());
  const int cv_type = (view.format() == PixelFormat::Gray8) ? CV_8UC1 : CV_8UC3;
  return Mat(view.height(), view.width(), cv_type,
             const_cast<std::uint8_t*>(view.data()), step);
}

int compute_block_size(int band_width, int band_height, double ratio) {
  const int effective_height =
      std::max(band_height, std::min(120, band_width / 11));
  const int size = std::max(3, static_cast<int>(effective_height * ratio));
  return size % 2 == 1 ? size : size + 1;
}

Mat compute_adaptive_mask_mat(const Mat& src, const Mat& gray,
                              const MaskConfig& config,
                              double* out_ratio,
                              Mat* out_hash_mask) {
  const int block_size =
      compute_block_size(gray.cols, gray.rows, config.block_size_ratio);
  if (!config.use_adaptive_mask) {
    Mat binary;
    const Mat kernel2 = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(2, 2));
    cv::adaptiveThreshold(gray, binary, 255, cv::ADAPTIVE_THRESH_GAUSSIAN_C,
                          cv::THRESH_BINARY_INV, block_size, config.adaptive_c);
    cv::morphologyEx(binary, binary, cv::MORPH_OPEN, kernel2);
    if (out_ratio != nullptr) {
      *out_ratio = cv::countNonZero(binary) / static_cast<double>(binary.total());
    }
    return binary;
  }

  const int h = src.rows;
  const int w = src.cols;
  const double scale = h / 120.0;

  const auto maps = detail::compute_polarity_contrast_maps(
      src, gray, block_size, config.adaptive_c, scale);

  Mat labels_pos, labels_neg;
  int num_labels_pos = 0, num_labels_neg = 0;
  const auto comps_pos = detail::filter_polarity_components(
      src, gray, maps.cand_pos, maps.dilated_dark_outline,
      maps.contrast_pos, true, labels_pos, num_labels_pos);
  const auto comps_neg = detail::filter_polarity_components(
      src, gray, maps.cand_neg, maps.dilated_bright_outline,
      maps.contrast_neg, false, labels_neg, num_labels_neg);

  Mat mask_bright, mask_dark;
  int chars_b = 0, area_b = 0;
  int chars_d = 0, area_d = 0;
  detail::cluster_polarity_text_lines(
      comps_pos, labels_pos, num_labels_pos, w, h, mask_bright, chars_b, area_b);
  detail::cluster_polarity_text_lines(
      comps_neg, labels_neg, num_labels_neg, w, h, mask_dark, chars_d, area_d);

  Mat valid_mask = (chars_b > 0 && chars_d > 0)
                       ? ((chars_b >= chars_d) ? mask_bright : mask_dark)
                   : (chars_b > 0) ? mask_bright
                   : (chars_d > 0) ? mask_dark
                                   : Mat::zeros(h, w, CV_8UC1);

  if (out_hash_mask != nullptr) {
    *out_hash_mask = valid_mask.clone();
  }

  const Mat kernel_h = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(3, 1));
  cv::morphologyEx(valid_mask, valid_mask, cv::MORPH_CLOSE, kernel_h);

  const double raw_ratio =
      cv::countNonZero(valid_mask) / static_cast<double>(valid_mask.total());
  const int total_chars = chars_b + chars_d;
  const int total_corridor = area_b + area_d;
  const int min_single_area = std::max(20, static_cast<int>(120 * scale * scale));
  const int min_multi_area = std::max(35, static_cast<int>(110 * scale * scale));

  double effective_ratio = 0.0;
  if ((total_chars >= 2 && total_corridor >= min_multi_area) ||
      (total_chars == 1 && total_corridor >= min_single_area)) {
    effective_ratio = std::max(raw_ratio, 0.015);
  } else {
    valid_mask = Mat::zeros(h, w, CV_8UC1);
  }

  if (out_ratio != nullptr) {
    *out_ratio = effective_ratio;
  }
  return valid_mask;
}

} // namespace

AdaptiveMaskResult compute_adaptive_presence_mask(
    const ImageView& image, const MaskConfig& config) {
  detail::ensure_opencv_single_thread();
  if (image.empty()) {
    return AdaptiveMaskResult{};
  }

  const Mat gray = to_gray_mat(image);
  const Mat src = to_src_mat(image);
  double ratio = 0.0;
  Mat hash_mask;
  const Mat binary =
      compute_adaptive_mask_mat(src, gray, config, &ratio, &hash_mask);

  const Mat& dhash_mat = hash_mask.empty() ? binary : hash_mask;
  const ImageView dhash_view(
      dhash_mat.ptr<std::uint8_t>(0),
      dhash_mat.cols,
      dhash_mat.rows,
      static_cast<int>(dhash_mat.step),
      PixelFormat::Gray8);
  const std::uint64_t dh = config.use_sector_features
                               ? compute_sector_dhash(dhash_view)
                               : compute_dhash(dhash_view, 8);

  ImageBuffer buf(binary.cols, binary.rows, PixelFormat::Gray8);
  for (int y = 0; y < binary.rows; ++y) {
    std::memcpy(buf.row(y), binary.ptr<std::uint8_t>(y), binary.cols);
  }

  return AdaptiveMaskResult{
      .valid_mask = std::move(buf),
      .foreground_ratio = ratio,
      .dhash = dh,
  };
}

} // namespace sublift::features
