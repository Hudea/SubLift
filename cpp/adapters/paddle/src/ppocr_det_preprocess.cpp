#include "ppocr_det_preprocess.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <stdexcept>

#if defined(SUBLIFT_HAS_OPENCV) || __has_include(<opencv2/core.hpp>)
#include <opencv2/core.hpp>
#include <opencv2/imgproc.hpp>
#define SUBLIFT_USE_OPENCV_FOR_DET_PREPROCESS 1
#endif

namespace sublift::adapters::paddle::detail {

namespace {

double read_positive_env(const char* name, double fallback) {
  if (const char* raw = std::getenv(name)) {
    char* end = nullptr;
    const double value = std::strtod(raw, &end);
    if (end != raw && value > 0.0) {
      return value;
    }
  }
  return fallback;
}

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
  return std::max<std::int32_t>(32, round_half_even(value / 32.0) * 32);
}

float normalize_channel(std::uint8_t value) {
  constexpr float kScale = 1.0F / 255.0F;
  constexpr float kMean = 0.5F;
  constexpr float kStd = 0.5F;
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
cv::Mat resize_within_bounds(const cv::Mat& source, double ratio) {
  const auto resize_h = std::max<std::int32_t>(
      1, static_cast<std::int32_t>(std::lround(source.rows * ratio)));
  const auto resize_w = std::max<std::int32_t>(
      1, static_cast<std::int32_t>(std::lround(source.cols * ratio)));
  cv::Mat resized;
  cv::resize(
      source, resized, cv::Size(resize_w, resize_h), 0.0, 0.0,
      cv::INTER_LINEAR);
  return resized;
}
#endif

}  // namespace

DetInputGeometry det_input_geometry_from_env() noexcept {
  return DetInputGeometry{
      .min_side = read_positive_env("SUBLIFT_DET_MIN_SIDE", 736.0),
      .pixel_budget = read_positive_env("SUBLIFT_DET_PIXEL_BUDGET", 0.0),
      .short_side = read_positive_env("SUBLIFT_DET_SHORT_SIDE", 0.0),
  };
}

DetInputSize det_input_size(std::int32_t width,
                            std::int32_t height,
                            const DetInputGeometry& geometry) noexcept {
  if (width <= 0 || height <= 0) {
    return {};
  }
  double ratio = 1.0;
  const double short_side = static_cast<double>(std::min(width, height));
  const double ref_width =
      std::max(static_cast<double>(width), geometry.reference_width);
  constexpr double kStripAspectThreshold = 8.0;
  constexpr double kLegacyStripPadAspect = 4.0;
  const bool is_horizontal_strip =
      ref_width / static_cast<double>(height) > kStripAspectThreshold;
  const double effective_short_side =
      is_horizontal_strip
          ? std::max(short_side, ref_width / kLegacyStripPadAspect)
          : short_side;

  if (geometry.pixel_budget > 0.0) {
    constexpr double kReferenceBandAspect = 6.0;
    const double effective_height = std::max(
        static_cast<double>(height), ref_width / kReferenceBandAspect);
    const double effective_area = ref_width * effective_height;
    if (effective_area < geometry.pixel_budget) {
      ratio = std::sqrt(geometry.pixel_budget / effective_area);
    }
    if (geometry.short_side > 0.0 &&
        effective_short_side * ratio < geometry.short_side) {
      ratio = geometry.short_side / effective_short_side;
    }
  } else if (geometry.min_side > 0.0 &&
             effective_short_side < geometry.min_side) {
    ratio = geometry.min_side / effective_short_side;
  }
  return DetInputSize{
      .width = multiple_of_32(
          static_cast<double>(static_cast<std::int32_t>(width * ratio))),
      .height = multiple_of_32(
          static_cast<double>(static_cast<std::int32_t>(height * ratio))),
  };
}

GlobalPreprocessResult prepare_global_image(
    const std::uint8_t* rgb,
    std::int32_t width,
    std::int32_t height,
    std::int32_t stride) {
  GlobalPreprocessResult result;
  prepare_global_image_into(rgb, width, height, stride, &result);
  return result;
}

void prepare_global_image_into(
    const std::uint8_t* rgb,
    std::int32_t width,
    std::int32_t height,
    std::int32_t stride,
    GlobalPreprocessResult* out) {
  if (rgb == nullptr || out == nullptr || width <= 0 || height <= 0 ||
      stride < width * 3) {
    throw std::invalid_argument("invalid RGB image for Paddle global preprocess");
  }

#if defined(SUBLIFT_USE_OPENCV_FOR_DET_PREPROCESS)
  cv::Mat source(
      height, width, CV_8UC3, const_cast<std::uint8_t*>(rgb),
      static_cast<std::size_t>(stride));
  cv::Mat working = source;

  constexpr double kMaxSide = 2000.0;
  constexpr double kMinSide = 30.0;
  if (std::max(working.rows, working.cols) > kMaxSide) {
    const double ratio =
        working.rows > working.cols ? kMaxSide / working.rows
                                    : kMaxSide / working.cols;
    working = resize_within_bounds(working, ratio);
  }
  if (working.cols < static_cast<int>(kMinSide)) {
    const double scale = std::min(
        kMinSide / static_cast<double>(working.cols),
        kMaxSide / static_cast<double>(std::max(working.rows, working.cols)));
    if (scale > 1.0) {
      working = resize_within_bounds(working, scale);
    }
  }
  const double ratio_h =
      static_cast<double>(height) / static_cast<double>(working.rows);
  const double ratio_w =
      static_cast<double>(width) / static_cast<double>(working.cols);

  constexpr double kVerticalRatio = 8.0;
  constexpr std::int32_t kVerticalMinHeight = 30;
  constexpr std::int32_t kNarrowStripMaxHeight = 140;
  constexpr double kNarrowStripMinAspect = 4.0;
  out->ratio_h = ratio_h;
  out->ratio_w = ratio_w;
  out->padding_left = 0;
  if (working.rows <= kVerticalMinHeight ||
      static_cast<double>(working.cols) / working.rows > kVerticalRatio ||
      (working.rows < kNarrowStripMaxHeight &&
       static_cast<double>(working.cols) / working.rows >
           kNarrowStripMinAspect)) {
    const bool is_full_hd_or_wider =
        working.cols >= 1600 || (working.cols >= 1300 && working.rows >= 64);
    const std::int32_t min_pad =
        is_full_hd_or_wider
            ? std::clamp<std::int32_t>(working.rows / 5, 12, 24)
            : std::clamp<std::int32_t>(working.rows / 6, 5, 16);
    const std::int32_t floor_height = is_full_hd_or_wider ? 96 : 64;
    const std::int32_t align = is_full_hd_or_wider ? 32 : 16;
    const std::int32_t raw_target =
        std::max(working.rows + 2 * min_pad, floor_height);
    const std::int32_t target_height =
        ((raw_target + align - 1) / align) * align;
    const std::int32_t total_pad =
        std::max<std::int32_t>(0, target_height - working.rows);
    const std::int32_t padding_top = total_pad / 2;
    const std::int32_t padding_bottom = total_pad - padding_top;

    out->width = working.cols;
    out->height = working.rows + total_pad;
    out->padding_top = padding_top;
    out->rgb.resize(static_cast<std::size_t>(out->width) *
                    static_cast<std::size_t>(out->height) * 3U);
    cv::Mat dst(out->height, out->width, CV_8UC3, out->rgb.data());
    cv::copyMakeBorder(
        working, dst, padding_top, padding_bottom, 0, 0, cv::BORDER_CONSTANT,
        cv::Scalar(0, 0, 0));
    return;
  }

  out->width = working.cols;
  out->height = working.rows;
  out->padding_top = 0;
  const auto packed_row_bytes =
      static_cast<std::size_t>(working.cols) * 3U;
  out->rgb.resize(packed_row_bytes * static_cast<std::size_t>(working.rows));
  for (int row = 0; row < working.rows; ++row) {
    std::memcpy(out->rgb.data() + static_cast<std::size_t>(row) *
                                      packed_row_bytes,
                working.ptr<std::uint8_t>(row), packed_row_bytes);
  }
#else
  throw std::runtime_error(
      "Paddle Det global preprocess requires an OpenCV-enabled build");
#endif
}

DetPreprocessResult prepare_det_input(
    const std::uint8_t* packed_rgb,
    std::int32_t width,
    std::int32_t height) {
  return prepare_det_input(packed_rgb, width, height,
                           det_input_geometry_from_env());
}

DetPreprocessResult prepare_det_input(
    const std::uint8_t* packed_rgb,
    std::int32_t width,
    std::int32_t height,
    const DetInputGeometry& geometry) {
  const DetInputSize size = det_input_size(width, height, geometry);
  if (size.width <= 0 || size.height <= 0) {
    throw std::runtime_error("Paddle Det resize produced an empty tensor");
  }
  DetPreprocessResult result;
  result.width = size.width;
  result.height = size.height;
  const auto plane =
      static_cast<std::size_t>(size.width) * static_cast<std::size_t>(size.height);
  result.nchw.resize(plane * 3);
  const auto actual = prepare_det_input_into(
      packed_rgb, width, height, geometry, result.nchw.data());
  if (actual != size) {
    throw std::runtime_error("Paddle Det preprocess geometry mismatch");
  }
  return result;
}

DetInputSize prepare_det_input_into(
    const std::uint8_t* packed_rgb,
    std::int32_t width,
    std::int32_t height,
    const DetInputGeometry& geometry,
    float* out_nchw) {
  if (packed_rgb == nullptr || width <= 0 || height <= 0 ||
      out_nchw == nullptr) {
    throw std::invalid_argument("invalid RGB image for Paddle Det preprocess");
  }

#if defined(SUBLIFT_USE_OPENCV_FOR_DET_PREPROCESS)
  const DetInputSize size = det_input_size(width, height, geometry);
  const auto resize_h = size.height;
  const auto resize_w = size.width;
  if (resize_h <= 0 || resize_w <= 0) {
    throw std::runtime_error("Paddle Det resize produced an empty tensor");
  }

  cv::Mat source(
      height, width, CV_8UC3, const_cast<std::uint8_t*>(packed_rgb));
  cv::Mat resized;
  if (resize_w == width && resize_h == height) {
    resized = source;
  } else {
    cv::resize(
        source, resized, cv::Size(resize_w, resize_h), 0.0, 0.0,
        cv::INTER_LINEAR);
  }

  const auto plane =
      static_cast<std::size_t>(resize_w) * static_cast<std::size_t>(resize_h);
  const auto& normalized = normalization_lut();
  cv::parallel_for_(cv::Range(0, resize_h), [&](const cv::Range& range) {
    for (std::int32_t y = range.start; y < range.end; ++y) {
      const auto* row = resized.ptr<std::uint8_t>(y);
      for (std::int32_t x = 0; x < resize_w; ++x) {
        const auto* pixel = row + static_cast<std::size_t>(x) * 3;
        const auto index =
            static_cast<std::size_t>(y) * static_cast<std::size_t>(resize_w) +
            static_cast<std::size_t>(x);
        out_nchw[index] = normalized[pixel[2]];
        out_nchw[plane + index] = normalized[pixel[1]];
        out_nchw[plane * 2 + index] = normalized[pixel[0]];
      }
    }
  });
  return size;
#else
  throw std::runtime_error(
      "Paddle Det preprocess requires an OpenCV-enabled build");
#endif
}

}  // namespace sublift::adapters::paddle::detail
