#include "ppocr_db_postprocess.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#if defined(SUBLIFT_HAS_OPENCV) || __has_include(<opencv2/opencv.hpp>)
#include <opencv2/opencv.hpp>
#define SUBLIFT_USE_OPENCV_FOR_DB 1
#endif

namespace sublift::paddle {

namespace {

struct Point2DF {
  float x{0.0f};
  float y{0.0f};
};

static float polygon_area(const std::vector<Point2DF>& poly) {
  if (poly.size() < 3) return 0.0f;
  float area = 0.0f;
  const size_t n = poly.size();
  for (size_t i = 0; i < n; ++i) {
    size_t j = (i + 1) % n;
    area += poly[i].x * poly[j].y;
    area -= poly[j].x * poly[i].y;
  }
  return std::abs(area) * 0.5f;
}

static float polygon_perimeter(const std::vector<Point2DF>& poly) {
  if (poly.size() < 2) return 0.0f;
  float perim = 0.0f;
  const size_t n = poly.size();
  for (size_t i = 0; i < n; ++i) {
    size_t j = (i + 1) % n;
    float dx = poly[j].x - poly[i].x;
    float dy = poly[j].y - poly[i].y;
    perim += std::sqrt(dx * dx + dy * dy);
  }
  return perim;
}

static std::vector<Point2DF> unclip_polygon(const std::vector<Point2DF>& poly, float unclip_ratio) {
  if (poly.size() < 3) return poly;
  float area = polygon_area(poly);
  float perim = polygon_perimeter(poly);
  if (perim < 1e-4f) return poly;

  float distance = area * unclip_ratio / perim;
  const size_t n = poly.size();
  std::vector<Point2DF> offset_poly(n);

  for (size_t i = 0; i < n; ++i) {
    size_t prev = (i + n - 1) % n;
    size_t next = (i + 1) % n;

    float v1x = poly[i].x - poly[prev].x;
    float v1y = poly[i].y - poly[prev].y;
    float len1 = std::sqrt(v1x * v1x + v1y * v1y);
    if (len1 > 1e-4f) { v1x /= len1; v1y /= len1; }

    float v2x = poly[next].x - poly[i].x;
    float v2y = poly[next].y - poly[i].y;
    float len2 = std::sqrt(v2x * v2x + v2y * v2y);
    if (len2 > 1e-4f) { v2x /= len2; v2y /= len2; }

    // Normal vectors (outward)
    float n1x = -v1y; float n1y = v1x;
    float n2x = -v2y; float n2y = v2x;

    float bisector_x = n1x + n2x;
    float bisector_y = n1y + n2y;
    float bisector_len = std::sqrt(bisector_x * bisector_x + bisector_y * bisector_y);
    if (bisector_len > 1e-4f) {
      bisector_x /= bisector_len;
      bisector_y /= bisector_len;
    }

    offset_poly[i].x = poly[i].x + bisector_x * distance;
    offset_poly[i].y = poly[i].y + bisector_y * distance;
  }
  return offset_poly;
}

}  // namespace

DBPostProcessResult db_postprocess(
    const float* prob_map_data,
    int map_h,
    int map_w,
    int orig_h,
    int orig_w,
    const DBPostProcessOptions& options) {

  DBPostProcessResult result;
  if (!prob_map_data || map_h <= 0 || map_w <= 0 || orig_h <= 0 || orig_w <= 0) {
    return result;
  }

#if defined(SUBLIFT_USE_OPENCV_FOR_DB)
  cv::Mat prob_map(map_h, map_w, CV_32FC1, const_cast<float*>(prob_map_data));

  // 1. Binarize (prob >= det_thresh)
  cv::Mat bitmap;
  cv::threshold(prob_map, bitmap, options.det_thresh, 255.0, cv::THRESH_BINARY);
  bitmap.convertTo(bitmap, CV_8UC1);

  // 2. Dilation (2x2 kernel)
  cv::Mat dilated_bitmap;
  cv::Mat kernel = cv::getStructuringElement(cv::MORPH_RECT, cv::Size(2, 2));
  cv::dilate(bitmap, dilated_bitmap, kernel);

  // 3. Find Contours
  std::vector<std::vector<cv::Point>> contours;
  cv::findContours(dilated_bitmap, contours, cv::RETR_LIST, cv::CHAIN_APPROX_SIMPLE);

  float rx = static_cast<float>(orig_w) / static_cast<float>(map_w);
  float ry = static_cast<float>(orig_h) / static_cast<float>(map_h);

  for (size_t i = 0; i < contours.size() && static_cast<int>(result.quads.size()) < options.max_candidates; ++i) {
    const auto& contour = contours[i];
    if (contour.size() < 3) continue;

    // Fast score: mean probability inside contour bounding rect
    cv::Rect rect = cv::boundingRect(contour);
    if (rect.width < options.min_size || rect.height < options.min_size) continue;

    cv::Mat mask = cv::Mat::zeros(rect.size(), CV_8UC1);
    std::vector<cv::Point> roi_contour;
    roi_contour.reserve(contour.size());
    for (const auto& pt : contour) {
      roi_contour.push_back(pt - rect.tl());
    }
    std::vector<std::vector<cv::Point>> roi_contours = {roi_contour};
    cv::drawContours(mask, roi_contours, 0, cv::Scalar(255), cv::FILLED);

    cv::Mat prob_roi = prob_map(rect);
    float score = static_cast<float>(cv::mean(prob_roi, mask)[0]);

    if (score < options.det_box_thresh) continue;

    // 4. Polygon Unclip
    cv::RotatedRect min_rect = cv::minAreaRect(contour);
    cv::Point2f pts[4];
    min_rect.points(pts);

    std::vector<Point2DF> poly = {
      {pts[0].x, pts[0].y},
      {pts[1].x, pts[1].y},
      {pts[2].x, pts[2].y},
      {pts[3].x, pts[3].y}
    };

    std::vector<Point2DF> unclipped = unclip_polygon(poly, options.unclip_ratio);

    // Re-fit minAreaRect on unclipped polygon
    std::vector<cv::Point2f> unclip_pts;
    for (const auto& p : unclipped) {
      unclip_pts.emplace_back(p.x, p.y);
    }
    cv::RotatedRect final_rect = cv::minAreaRect(unclip_pts);
    cv::Point2f final_pts[4];
    final_rect.points(final_pts);

    // Map coordinates back to original image size
    QuadPolygon quad;
    quad.p0 = Point2D{final_pts[0].x * rx, final_pts[0].y * ry};
    quad.p1 = Point2D{final_pts[1].x * rx, final_pts[1].y * ry};
    quad.p2 = Point2D{final_pts[2].x * rx, final_pts[2].y * ry};
    quad.p3 = Point2D{final_pts[3].x * rx, final_pts[3].y * ry};
    quad.score = score;

    result.quads.push_back(quad);
    result.aabbs.push_back(quad_to_aabb({quad.p0, quad.p1, quad.p2, quad.p3}));
  }
#else
  // Fallback if compiled without OpenCV
  float rx = static_cast<float>(orig_w) / static_cast<float>(map_w);
  float ry = static_cast<float>(orig_h) / static_cast<float>(map_h);
  for (int y = 0; y < map_h; ++y) {
    for (int x = 0; x < map_w; ++x) {
      float val = prob_map_data[y * map_w + x];
      if (val >= options.det_box_thresh) {
        QuadPolygon quad;
        quad.p0 = Point2D{x * rx, y * ry};
        quad.p1 = Point2D{(x + 10.0f) * rx, y * ry};
        quad.p2 = Point2D{(x + 10.0f) * rx, (y + 10.0f) * ry};
        quad.p3 = Point2D{x * rx, (y + 10.0f) * ry};
        quad.score = val;
        result.quads.push_back(quad);
        result.aabbs.push_back(quad_to_aabb({quad.p0, quad.p1, quad.p2, quad.p3}));
        return result;
      }
    }
  }
#endif

  return result;
}

}  // namespace sublift::paddle
