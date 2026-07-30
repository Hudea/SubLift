#include "ppocr_crop_cls.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <stdexcept>

#if defined(SUBLIFT_HAS_OPENCV) || __has_include(<opencv2/opencv.hpp>)
#include <opencv2/opencv.hpp>
#define SUBLIFT_USE_OPENCV_FOR_CROP 1
#endif

namespace sublift::paddle {

CropResult get_rotate_crop(
    const uint8_t* src_rgb,
    int src_w,
    int src_h,
    int stride,
    const QuadPolygon& quad) {

  CropResult result;
  if (!src_rgb || src_w <= 0 || src_h <= 0) return result;

#if defined(SUBLIFT_USE_OPENCV_FOR_CROP)
  cv::Mat src_img(src_h, src_w, CV_8UC3, const_cast<uint8_t*>(src_rgb), stride);

  cv::Point2f src_pts[4] = {
    {static_cast<float>(quad.p0.x), static_cast<float>(quad.p0.y)},
    {static_cast<float>(quad.p1.x), static_cast<float>(quad.p1.y)},
    {static_cast<float>(quad.p2.x), static_cast<float>(quad.p2.y)},
    {static_cast<float>(quad.p3.x), static_cast<float>(quad.p3.y)}
  };

  float w1 = std::hypot(src_pts[1].x - src_pts[0].x, src_pts[1].y - src_pts[0].y);
  float w2 = std::hypot(src_pts[2].x - src_pts[3].x, src_pts[2].y - src_pts[3].y);
  int crop_w = std::max(2, static_cast<int>(std::max(w1, w2)));

  float h1 = std::hypot(src_pts[3].x - src_pts[0].x, src_pts[3].y - src_pts[0].y);
  float h2 = std::hypot(src_pts[2].x - src_pts[1].x, src_pts[2].y - src_pts[1].y);
  int crop_h = std::max(2, static_cast<int>(std::max(h1, h2)));

  cv::Point2f dst_pts[4] = {
    {0.0f, 0.0f},
    {static_cast<float>(crop_w), 0.0f},
    {static_cast<float>(crop_w), static_cast<float>(crop_h)},
    {0.0f, static_cast<float>(crop_h)}
  };

  cv::Mat M = cv::getPerspectiveTransform(src_pts, dst_pts);
  cv::Mat crop;
  cv::warpPerspective(src_img, crop, M, cv::Size(crop_w, crop_h), cv::INTER_CUBIC, cv::BORDER_REPLICATE);

  // NumPy rot90 used by RapidOCR rotates counter-clockwise.
  if (static_cast<float>(crop_h) / static_cast<float>(crop_w) >= 1.5f) {
    cv::rotate(crop, crop, cv::ROTATE_90_COUNTERCLOCKWISE);
    result.rotated_90 = true;
    std::swap(crop_w, crop_h);
  }

  result.width = crop.cols;
  result.height = crop.rows;
  result.rgb_data.resize(static_cast<size_t>(result.width * result.height * 3));
  if (crop.isContinuous()) {
    std::memcpy(result.rgb_data.data(), crop.data, result.rgb_data.size());
  } else {
    for (int y = 0; y < result.height; ++y) {
      std::memcpy(result.rgb_data.data() + y * result.width * 3, crop.ptr(y), static_cast<size_t>(result.width * 3));
    }
  }
#else
  // Fallback AABB crop if compiled without OpenCV
  int x0 = std::clamp(static_cast<int>(quad.p0.x), 0, src_w - 1);
  int y0 = std::clamp(static_cast<int>(quad.p0.y), 0, src_h - 1);
  int x1 = std::clamp(static_cast<int>(quad.p2.x), x0 + 1, src_w);
  int y1 = std::clamp(static_cast<int>(quad.p2.y), y0 + 1, src_h);
  result.width = x1 - x0;
  result.height = y1 - y0;
  result.rgb_data.resize(static_cast<size_t>(result.width * result.height * 3));
  for (int y = 0; y < result.height; ++y) {
    const uint8_t* sp = src_rgb + (y0 + y) * stride + x0 * 3;
    uint8_t* dp = result.rgb_data.data() + y * result.width * 3;
    std::memcpy(dp, sp, static_cast<size_t>(result.width * 3));
  }
#endif

  return result;
}

namespace {

std::vector<std::uint8_t> resize_linear_u8(
    const CropResult& crop,
    int target_width,
    int target_height) {
  if (crop.width == target_width && crop.height == target_height) {
    return crop.rgb_data;
  }

  // OpenCV INTER_LINEAR's uint8 path uses 11-bit fixed-point interpolation
  // coefficients. Implement that scalar contract here so tensors do not
  // depend on the SIMD/rounding choices of a particular OpenCV minor release.
  static constexpr int kCoefficientBits = 11;
  static constexpr int kCoefficientScale = 1 << kCoefficientBits;

  struct Coefficients {
    int source{0};
    int first{kCoefficientScale};
    int second{0};
  };
  const auto build_coefficients = [](int source_size, int target_size) {
    std::vector<Coefficients> result(
        static_cast<std::size_t>(target_size));
    const double inverse_scale =
        static_cast<double>(source_size) / static_cast<double>(target_size);
    for (int target = 0; target < target_size; ++target) {
      float position = static_cast<float>(
          (static_cast<double>(target) + 0.5) * inverse_scale - 0.5);
      int source = static_cast<int>(std::floor(position));
      position -= static_cast<float>(source);
      if (source < 0) {
        source = 0;
        position = 0.0F;
      }
      if (source >= source_size - 1) {
        source = source_size - 1;
        position = 0.0F;
      }
      const int second = static_cast<int>(
          std::lrint(position * static_cast<float>(kCoefficientScale)));
      result[static_cast<std::size_t>(target)] = Coefficients{
          .source = source,
          .first = kCoefficientScale - second,
          .second = second,
      };
    }
    return result;
  };

  const auto horizontal = build_coefficients(crop.width, target_width);
  const auto vertical = build_coefficients(crop.height, target_height);
  std::vector<std::uint8_t> resized(
      static_cast<std::size_t>(target_width) *
      static_cast<std::size_t>(target_height) * 3);
  for (int y = 0; y < target_height; ++y) {
    const auto& vertical_coeff = vertical[static_cast<std::size_t>(y)];
    const int next_y =
        std::min(vertical_coeff.source + 1, crop.height - 1);
    for (int x = 0; x < target_width; ++x) {
      const auto& horizontal_coeff =
          horizontal[static_cast<std::size_t>(x)];
      const int next_x =
          std::min(horizontal_coeff.source + 1, crop.width - 1);
      for (int channel = 0; channel < 3; ++channel) {
        const auto top_left = crop.rgb_data[
            (vertical_coeff.source * crop.width +
             horizontal_coeff.source) *
                3 +
            channel];
        const auto top_right = crop.rgb_data[
            (vertical_coeff.source * crop.width + next_x) * 3 + channel];
        const auto bottom_left = crop.rgb_data[
            (next_y * crop.width + horizontal_coeff.source) * 3 + channel];
        const auto bottom_right = crop.rgb_data[
            (next_y * crop.width + next_x) * 3 + channel];
        const std::int64_t top =
            static_cast<std::int64_t>(top_left) * horizontal_coeff.first +
            static_cast<std::int64_t>(top_right) * horizontal_coeff.second;
        const std::int64_t bottom =
            static_cast<std::int64_t>(bottom_left) * horizontal_coeff.first +
            static_cast<std::int64_t>(bottom_right) * horizontal_coeff.second;
        // Match OpenCV's specialized uint8 vertical-linear cast. It shifts
        // horizontal intermediates before multiplying to keep SIMD lanes
        // narrow; reproducing those operation boundaries is required for
        // byte-identical RapidOCR tensors.
        const auto interpolated =
            (((static_cast<std::int64_t>(vertical_coeff.first) *
               (top >> 4)) >>
              16) +
             ((static_cast<std::int64_t>(vertical_coeff.second) *
               (bottom >> 4)) >>
              16) +
             2) >>
            2;
        resized[
            (y * target_width + x) * 3 + channel] =
            static_cast<std::uint8_t>(
                std::clamp<std::int64_t>(interpolated, 0, 255));
      }
    }
  }
  return resized;
}

float normalize_pixel(std::uint8_t pixel) {
  // Preserve NumPy float32 operation boundaries and avoid compiler FMA
  // contraction changing audited tensors.
  volatile float value = static_cast<float>(pixel);
  value = value / 255.0F;
  value = value - 0.5F;
  value = value / 0.5F;
  return value;
}

const std::array<float, 256>& normalization_lut() {
  static const std::array<float, 256> values = [] {
    std::array<float, 256> result{};
    for (std::size_t index = 0; index < result.size(); ++index) {
      result[index] = normalize_pixel(static_cast<std::uint8_t>(index));
    }
    return result;
  }();
  return values;
}

void prepare_padded_tensor(
    const CropResult& crop,
    int target_height,
    int target_width,
    float* out_nchw_tensor) {
  if (out_nchw_tensor == nullptr ||
      target_height <= 0 || target_width <= 0) {
    return;
  }
  std::memset(
      out_nchw_tensor,
      0,
      static_cast<std::size_t>(3) *
          static_cast<std::size_t>(target_height) *
          static_cast<std::size_t>(target_width) * sizeof(float));
  if (crop.width <= 0 || crop.height <= 0 || crop.rgb_data.empty()) {
    return;
  }

  const int valid_width =
      resized_valid_width(crop, target_height, target_width);

  const auto resized =
      resize_linear_u8(crop, valid_width, target_height);
  const auto& normalized = normalization_lut();
  for (int channel = 0; channel < 3; ++channel) {
    const int rgb_channel = 2 - channel;
    for (int y = 0; y < target_height; ++y) {
      for (int x = 0; x < valid_width; ++x) {
        out_nchw_tensor[
            channel * (target_height * target_width) +
            y * target_width + x] =
            normalized[resized[
                (y * valid_width + x) * 3 + rgb_channel]];
      }
    }
  }
}

}  // namespace

int resized_valid_width(
    const CropResult& crop,
    int target_height,
    int target_width) {
  if (crop.width <= 0 || crop.height <= 0 ||
      target_height <= 0 || target_width <= 0) {
    return 0;
  }
  const auto ratio =
      static_cast<double>(crop.width) / static_cast<double>(crop.height);
  return std::min(
      target_width,
      std::max(
          1,
          static_cast<int>(
              std::ceil(static_cast<double>(target_height) * ratio))));
}

void prepare_cls_tensor(const CropResult& crop, float* out_nchw_tensor) {
  prepare_padded_tensor(crop, 48, 192, out_nchw_tensor);
}

ClsResult apply_cls_result(
    CropResult& crop,
    int label,
    float score,
    float cls_thresh) {
  ClsResult result{
      .label = label,
      .score = score,
      .rotated_180 = false,
  };
  if (label != 1 || score <= cls_thresh ||
      crop.width <= 0 || crop.height <= 0 || crop.rgb_data.empty()) {
    return result;
  }

#if defined(SUBLIFT_USE_OPENCV_FOR_CROP)
  cv::Mat image(crop.height, crop.width, CV_8UC3, crop.rgb_data.data());
  cv::rotate(image, image, cv::ROTATE_180);
#else
  std::vector<std::uint8_t> rotated(crop.rgb_data.size());
  const auto pixel_count =
      static_cast<std::size_t>(crop.width) * static_cast<std::size_t>(crop.height);
  for (std::size_t index = 0; index < pixel_count; ++index) {
    const auto source = (pixel_count - 1 - index) * 3;
    const auto target = index * 3;
    std::memcpy(rotated.data() + target, crop.rgb_data.data() + source, 3);
  }
  crop.rgb_data = std::move(rotated);
#endif
  result.rotated_180 = true;
  return result;
}

void prepare_rec_tensor(
    const CropResult& crop,
    int rec_h,
    int target_w,
    float* out_nchw_tensor) {
  prepare_padded_tensor(crop, rec_h, target_w, out_nchw_tensor);
}

}  // namespace sublift::paddle
