#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "sublift/models.hpp"
#include "sublift/paddle_geometry.hpp"

namespace sublift::paddle {

struct QuadPolygon {
  Point2D p0;
  Point2D p1;
  Point2D p2;
  Point2D p3;
  float score{0.0f};
};

struct DBPostProcessOptions {
  float det_thresh{0.3f};
  float det_box_thresh{0.5f};
  float unclip_ratio{1.6f};
  std::string score_mode{"fast"};
  bool use_dilation{true};
  int min_size{3};
  int max_candidates{1000};
};

struct DBPostProcessResult {
  std::vector<QuadPolygon> quads;
  std::vector<OcrCropBox> aabbs;
};

/// Perform DB (Differentiable Binarization) postprocessing on a probability map:
/// 1. Threshold binarization (prob > det_thresh)
/// 2. 2x2 Kernel Dilation
/// 3. Contour extraction & polygon minAreaRect fitting
/// 4. Polygon box score calculation & thresholding (score >= det_box_thresh)
/// 5. Polygon unclip offset (D = Area * unclip_ratio / Perimeter)
/// 6. Rescale back to original image coordinates
[[nodiscard]] DBPostProcessResult db_postprocess(
    const float* prob_map_data,
    int map_h,
    int map_w,
    int orig_h,
    int orig_w,
    const DBPostProcessOptions& options = {});

/// Mirror TextDetector.sorted_boxes after DBPostProcess. RapidOCR sorts only
/// box coordinates and leaves its parallel score list positional; this helper
/// preserves that observable behavior.
void sort_db_result(DBPostProcessResult* result);

}  // namespace sublift::paddle
