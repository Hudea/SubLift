#include <algorithm>
#include <bit>
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#include <sublift/features/dhash.hpp>

namespace sublift::features {

namespace {

cv::Mat image_view_to_gray_mat(const ImageView& view) {
  if (view.empty()) {
    return cv::Mat();
  }
  const int cv_type = (view.format() == PixelFormat::RGB24) ? CV_8UC3 : CV_8UC1;
  cv::Mat src(view.height(), view.width(), cv_type,
              const_cast<std::uint8_t*>(view.data()),
              static_cast<std::size_t>(view.stride_bytes()));
  if (view.format() == PixelFormat::RGB24) {
    cv::Mat gray;
    cv::cvtColor(src, gray, cv::COLOR_RGB2GRAY);
    return gray;
  }
  return src;
}

std::uint64_t compute_rect_dhash_mat(const cv::Mat& gray, int cols, int rows) {
  if (gray.empty() || gray.cols < 2 || gray.rows < 1 || cols <= 0 || rows <= 0) {
    return 0ULL;
  }
  cv::Mat resized;
  cv::resize(gray, resized, cv::Size(cols + 1, rows), 0, 0, cv::INTER_AREA);

  std::uint64_t hash_value = 0ULL;
  for (int y = 0; y < rows; ++y) {
    const std::uint8_t* row = resized.ptr<std::uint8_t>(y);
    for (int x = 0; x < cols; ++x) {
      hash_value = (hash_value << 1) | (row[x + 1] > row[x] ? 1ULL : 0ULL);
    }
  }
  return hash_value;
}

} // namespace

std::uint64_t compute_dhash(const ImageView& image, int hash_size) {
  if (image.empty() || hash_size <= 0 || hash_size > 8) {
    return 0ULL;
  }
  const cv::Mat gray = image_view_to_gray_mat(image);
  return compute_rect_dhash_mat(gray, hash_size, hash_size);
}

std::uint64_t compute_sector_dhash(const ImageView& image) {
  if (image.empty() || image.width() < 10 || image.height() < 1) {
    return 0ULL;
  }
  const cv::Mat gray = image_view_to_gray_mat(image);
  const int w = gray.cols;
  const int l_cut = std::clamp(static_cast<int>(0.28 * w), 1, w - 2);
  const int r_cut = std::clamp(static_cast<int>(0.72 * w), l_cut + 1, w - 1);

  const std::uint64_t h_left = compute_rect_dhash_mat(gray.colRange(0, l_cut), 12, 2);
  const std::uint64_t h_center = compute_rect_dhash_mat(gray.colRange(l_cut, r_cut), 4, 4);
  const std::uint64_t h_right = compute_rect_dhash_mat(gray.colRange(r_cut, w), 12, 2);

  return ((h_left & 0xFFFFFFULL) << 40) |
         ((h_center & 0xFFFFULL) << 24) |
         (h_right & 0xFFFFFFULL);
}

int hamming_distance(std::uint64_t h1, std::uint64_t h2) noexcept {
  return std::popcount(h1 ^ h2);
}

} // namespace sublift::features
