#include <algorithm>
#include <cstring>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <sublift/features/mask.hpp>

#include "opencv_thread_guard.hpp"

namespace sublift::features {

namespace {

cv::Mat image_view_to_mat(const ImageView& view) {
  if (view.empty()) {
    return cv::Mat();
  }
  int cv_type = (view.format() == PixelFormat::RGB24) ? CV_8UC3 : CV_8UC1;
  // view.data() is const uint8_t*, create const Mat without copying
  return cv::Mat(view.height(), view.width(), cv_type,
                 const_cast<std::uint8_t*>(view.data()),
                 static_cast<std::size_t>(view.stride_bytes()));
}

} // namespace

ImageBuffer compute_binary_mask(const ImageView& image, const MaskConfig& config) {
  detail::ensure_opencv_single_thread();
  if (image.empty()) {
    return ImageBuffer{};
  }

  cv::Mat src = image_view_to_mat(image);
  cv::Mat gray;
  if (image.format() == PixelFormat::RGB24) {
    cv::cvtColor(src, gray, cv::COLOR_RGB2GRAY);
  } else {
    gray = src;
  }

  const int effective_height = std::max(gray.rows, std::min(120, gray.cols / 11));
  int block_size = config.adaptive_block_size > 0
                       ? config.adaptive_block_size
                       : std::max(3, static_cast<int>(effective_height * config.block_size_ratio));
  if (block_size % 2 == 0) {
    block_size += 1;
  }
  if (block_size < 3) {
    block_size = 3;
  }

  cv::Mat binary;
  cv::adaptiveThreshold(gray, binary, 255.0, cv::ADAPTIVE_THRESH_GAUSSIAN_C,
                        cv::THRESH_BINARY_INV, block_size, config.adaptive_c);

  int kw = std::max(1, config.morph_kernel_width);
  int kh = std::max(1, config.morph_kernel_height);
  cv::Mat kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(kw, kh));
  cv::morphologyEx(binary, binary, cv::MORPH_OPEN, kernel);

  ImageBuffer out(image.width(), image.height(), PixelFormat::Gray8);
  for (int y = 0; y < image.height(); ++y) {
    std::memcpy(out.row(y), binary.ptr<std::uint8_t>(y), image.width());
  }
  return out;
}

double compute_foreground_ratio(const ImageView& mask) {
  if (mask.empty()) {
    return 0.0;
  }
  std::size_t fg_count = 0;
  for (int y = 0; y < mask.height(); ++y) {
    const std::uint8_t* row = mask.row(y);
    for (int x = 0; x < mask.width(); ++x) {
      if (row[x] > 0) {
        ++fg_count;
      }
    }
  }
  const double total_pixels = static_cast<double>(mask.width()) * static_cast<double>(mask.height());
  if (total_pixels <= 0.0) {
    return 0.0;
  }
  return static_cast<double>(fg_count) / total_pixels;
}

} // namespace sublift::features
