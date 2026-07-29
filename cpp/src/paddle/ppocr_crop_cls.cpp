#include "ppocr_crop_cls.hpp"

#include <algorithm>
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

  // High-narrow check: rotate 90 degrees if crop_h / crop_w >= 1.5
  if (static_cast<float>(crop_h) / static_cast<float>(crop_w) >= 1.5f) {
    cv::rotate(crop, crop, cv::ROTATE_90_CLOCKWISE);
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

ClsResult process_cls(CropResult& crop, float cls_thresh) {
  ClsResult res;
  if (crop.width <= 0 || crop.height <= 0 || crop.rgb_data.empty()) return res;

#if defined(SUBLIFT_USE_OPENCV_FOR_CROP)
  // Dummy Cls check wrapper for parity contract
  if (res.label == 1 && res.score >= cls_thresh) {
    cv::Mat crop_mat(crop.height, crop.width, CV_8UC3, crop.rgb_data.data());
    cv::rotate(crop_mat, crop_mat, cv::ROTATE_180);
    res.rotated_180 = true;
  }
#endif
  return res;
}

void prepare_rec_tensor(
    const CropResult& crop,
    int rec_h,
    int target_w,
    float* out_nchw_tensor) {

  if (!out_nchw_tensor || rec_h <= 0 || target_w <= 0) return;
  std::memset(out_nchw_tensor, 0, static_cast<size_t>(3 * rec_h * target_w) * sizeof(float));

  if (crop.width <= 0 || crop.height <= 0 || crop.rgb_data.empty()) return;

  float wh_ratio = static_cast<float>(crop.width) / static_cast<float>(crop.height);
  int valid_w = std::max(1, static_cast<int>(std::ceil(rec_h * wh_ratio)));
  valid_w = std::min(valid_w, target_w);

#if defined(SUBLIFT_USE_OPENCV_FOR_CROP)
  cv::Mat src_mat(crop.height, crop.width, CV_8UC3, const_cast<uint8_t*>(crop.rgb_data.data()));
  cv::Mat resized;
  cv::resize(src_mat, resized, cv::Size(valid_w, rec_h), 0, 0, cv::INTER_LINEAR);

  // Convert BGR24 & Normalize float (val / 127.5 - 1.0) into NCHW
  for (int c = 0; c < 3; ++c) {
    int src_c = 2 - c; // RGB to BGR
    for (int y = 0; y < rec_h; ++y) {
      const uint8_t* ptr = resized.ptr<uint8_t>(y);
      for (int x = 0; x < valid_w; ++x) {
        float val = static_cast<float>(ptr[x * 3 + src_c]) / 127.5f - 1.0f;
        out_nchw_tensor[c * (rec_h * target_w) + y * target_w + x] = val;
      }
    }
  }
#else
  // Fallback simple scaling
  for (int c = 0; c < 3; ++c) {
    for (int y = 0; y < rec_h; ++y) {
      for (int x = 0; x < valid_w; ++x) {
        int sx = x * crop.width / valid_w;
        int sy = y * crop.height / rec_h;
        uint8_t val_bgr = crop.rgb_data[(sy * crop.width + sx) * 3 + (2 - c)];
        out_nchw_tensor[c * (rec_h * target_w) + y * target_w + x] = static_cast<float>(val_bgr) / 127.5f - 1.0f;
      }
    }
  }
#endif
}

}  // namespace sublift::paddle
