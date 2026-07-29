#pragma once

#include <vector>
#include "ppocr_db_postprocess.hpp"

namespace sublift::paddle {

struct CropResult {
  std::vector<uint8_t> rgb_data;
  int width{0};
  int height{0};
  bool rotated_90{false};
};

/// Perform perspective transform crop on a quad (p0, p1, p2, p3):
/// 1. Compute crop width and height
/// 2. Apply getPerspectiveTransform & warpPerspective
/// 3. If crop_h / crop_w >= 1.5, rotate 90 degrees clockwise to horizontal
[[nodiscard]] CropResult get_rotate_crop(
    const uint8_t* src_rgb,
    int src_w,
    int src_h,
    int stride,
    const QuadPolygon& quad);

struct ClsResult {
  int label{0};        // 0: 0 deg, 1: 180 deg
  float score{0.0f};   // confidence
  bool rotated_180{false};
};

/// Perform 180 degree direction classification check and rotation:
/// Resize to 48x192, normalize NCHW float tensor, softmax check label == 1 and score >= thresh (default 0.9)
[[nodiscard]] ClsResult process_cls(
    CropResult& crop,
    float cls_thresh = 0.9f);

/// Rec image preprocessing with right-zero padding:
/// Resizes crop to height 48, computes target width, pads right side with 0.0 float tensor values.
void prepare_rec_tensor(
    const CropResult& crop,
    int rec_h,
    int target_w,
    float* out_nchw_tensor);

}  // namespace sublift::paddle
