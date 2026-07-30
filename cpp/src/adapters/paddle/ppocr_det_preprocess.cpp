#include "ppocr_det_preprocess.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>

#if defined(SUBLIFT_HAS_OPENCV) || __has_include(<opencv2/opencv.hpp>)
#include <opencv2/opencv.hpp>
#define SUBLIFT_USE_OPENCV_FOR_DET_PREPROCESS 1
#endif

namespace sublift::paddle {

namespace {

std::int32_t round_half_even(double value) {
  const double lower = std::floor(value);
  const double fraction = value - lower;
  if (fraction < 0.5) {
    return static_cast<std::int32_t>(lower);
  }
  if (fraction > 0.5) {
    return static_cast<std::int32_t>(lower + 1.0);
  }
  const auto integer = static_cast<std::int64_t>(lower);
  return static_cast<std::int32_t>(
      integer % 2 == 0 ? integer : integer + 1);
}

std::int32_t multiple_of_32(double value) {
  return round_half_even(value / 32.0) * 32;
}

float normalize_channel(std::uint8_t value) {
  constexpr float kScale = 1.0F / 255.0F;
  constexpr float kMean = 0.5F;
  constexpr float kStd = 0.5F;
  // Preserve NumPy's three separate float32 ufunc operations. Without these
  // sequence points an optimizing compiler may contract multiply/subtract.
  volatile float scaled = static_cast<float>(value) * kScale;
  volatile float centered = scaled - kMean;
  return centered / kStd;
}

const std::array<float, 256>& normalization_lut() {
  static const std::array<float, 256> values = [] {
    std::array<float, 256> result{};
    for (std::size_t index = 0; index < result.size(); ++index) {
      result[index] =
          normalize_channel(static_cast<std::uint8_t>(index));
    }
    return result;
  }();
  return values;
}

#if defined(SUBLIFT_USE_OPENCV_FOR_DET_PREPROCESS)
cv::Mat resize_with_ratio(
    const cv::Mat& source,
    double ratio,
    double* ratio_h,
    double* ratio_w) {
  const auto resize_h = multiple_of_32(
      static_cast<double>(static_cast<std::int32_t>(source.rows * ratio)));
  const auto resize_w = multiple_of_32(
      static_cast<double>(static_cast<std::int32_t>(source.cols * ratio)));
  if (resize_h <= 0 || resize_w <= 0) {
    throw std::runtime_error("Paddle Det global resize produced an empty image");
  }
  cv::Mat resized;
  cv::resize(
      source, resized, cv::Size(resize_w, resize_h), 0.0, 0.0,
      cv::INTER_LINEAR);
  *ratio_h = static_cast<double>(source.rows) / resize_h;
  *ratio_w = static_cast<double>(source.cols) / resize_w;
  return resized;
}
#endif

}  // namespace

GlobalPreprocessResult prepare_global_image(
    const std::uint8_t* rgb,
    std::int32_t width,
    std::int32_t height,
    std::int32_t stride) {
  if (rgb == nullptr || width <= 0 || height <= 0 || stride < width * 3) {
    throw std::invalid_argument("invalid RGB image for Paddle global preprocess");
  }

#if defined(SUBLIFT_USE_OPENCV_FOR_DET_PREPROCESS)
  cv::Mat source(
      height, width, CV_8UC3, const_cast<std::uint8_t*>(rgb),
      static_cast<std::size_t>(stride));
  cv::Mat working = source.clone();
  double ratio_h = 1.0;
  double ratio_w = 1.0;

  constexpr double kMaxSide = 2000.0;
  constexpr double kMinSide = 30.0;
  if (std::max(working.rows, working.cols) > kMaxSide) {
    const double ratio =
        working.rows > working.cols ? kMaxSide / working.rows
                                    : kMaxSide / working.cols;
    working = resize_with_ratio(working, ratio, &ratio_h, &ratio_w);
  }
  if (std::min(working.rows, working.cols) < kMinSide) {
    const double ratio =
        working.rows < working.cols ? kMinSide / working.rows
                                    : kMinSide / working.cols;
    // RapidOCR 3.9.2 overwrites rather than composes these ratios.
    working = resize_with_ratio(working, ratio, &ratio_h, &ratio_w);
  }

  constexpr double kVerticalRatio = 8.0;
  constexpr std::int32_t kVerticalMinHeight = 30;
  std::int32_t padding_top = 0;
  if (working.rows <= kVerticalMinHeight ||
      static_cast<double>(working.cols) / working.rows > kVerticalRatio) {
    const auto target_height =
        std::max(
            static_cast<std::int32_t>(working.cols / kVerticalRatio),
            kVerticalMinHeight) *
        2;
    padding_top = std::abs(target_height - working.rows) / 2;
    cv::Mat padded;
    cv::copyMakeBorder(
        working, padded, padding_top, padding_top, 0, 0, cv::BORDER_CONSTANT,
        cv::Scalar(0, 0, 0));
    working = std::move(padded);
  }

  GlobalPreprocessResult result;
  result.width = working.cols;
  result.height = working.rows;
  result.ratio_h = ratio_h;
  result.ratio_w = ratio_w;
  result.padding_top = padding_top;
  result.rgb.assign(
      working.data,
      working.data + static_cast<std::size_t>(working.total()) * 3);
  return result;
#else
  throw std::runtime_error(
      "Paddle Det global preprocess requires an OpenCV-enabled build");
#endif
}

DetPreprocessResult prepare_det_input(
    const std::uint8_t* packed_rgb,
    std::int32_t width,
    std::int32_t height) {
  if (packed_rgb == nullptr || width <= 0 || height <= 0) {
    throw std::invalid_argument("invalid RGB image for Paddle Det preprocess");
  }

#if defined(SUBLIFT_USE_OPENCV_FOR_DET_PREPROCESS)
  constexpr double kLimitSide = 736.0;
  double ratio = 1.0;
  if (std::min(width, height) < kLimitSide) {
    ratio = width < height ? kLimitSide / width : kLimitSide / height;
  }
  const auto resize_h = multiple_of_32(
      static_cast<double>(static_cast<std::int32_t>(height * ratio)));
  const auto resize_w = multiple_of_32(
      static_cast<double>(static_cast<std::int32_t>(width * ratio)));
  if (resize_h <= 0 || resize_w <= 0) {
    throw std::runtime_error("Paddle Det resize produced an empty tensor");
  }

  cv::Mat source(
      height, width, CV_8UC3, const_cast<std::uint8_t*>(packed_rgb));
  cv::Mat resized;
  cv::resize(
      source, resized, cv::Size(resize_w, resize_h), 0.0, 0.0,
      cv::INTER_LINEAR);

  DetPreprocessResult result;
  result.width = resize_w;
  result.height = resize_h;
  const auto plane = static_cast<std::size_t>(resize_w) * resize_h;
  result.nchw.resize(plane * 3);
  const auto& normalized = normalization_lut();
  for (std::int32_t y = 0; y < resize_h; ++y) {
    const auto* row = resized.ptr<std::uint8_t>(y);
    for (std::int32_t x = 0; x < resize_w; ++x) {
      const auto* pixel = row + static_cast<std::size_t>(x) * 3;
      const auto index =
          static_cast<std::size_t>(y) * resize_w + x;
      // RapidOCR loads BGR. The source here is RGB, so reverse channels while
      // producing NCHW. NumPy keeps the image multiply/subtract/divide in
      // float32 even though the resulting temporary array is typed float64.
      result.nchw[index] = normalized[pixel[2]];
      result.nchw[plane + index] = normalized[pixel[1]];
      result.nchw[plane * 2 + index] = normalized[pixel[0]];
    }
  }
  return result;
#else
  throw std::runtime_error(
      "Paddle Det preprocess requires an OpenCV-enabled build");
#endif
}

}  // namespace sublift::paddle
