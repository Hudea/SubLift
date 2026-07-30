#include "ppocr_db_postprocess.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <vector>

#if defined(SUBLIFT_HAS_OPENCV) || __has_include(<opencv2/opencv.hpp>)
#include <opencv2/opencv.hpp>
#define SUBLIFT_USE_OPENCV_FOR_DB 1
#endif

namespace sublift::paddle {

namespace {

#if defined(SUBLIFT_USE_OPENCV_FOR_DB)

struct MiniBox {
  std::array<cv::Point2f, 4> points{};
  cv::RotatedRect rectangle;
  float short_side{0.0F};
};

MiniBox get_mini_box(cv::InputArray contour) {
  MiniBox result;
  result.rectangle = cv::minAreaRect(contour);
  std::array<cv::Point2f, 4> raw{};
  result.rectangle.points(raw.data());
  std::stable_sort(
      raw.begin(), raw.end(),
      [](const cv::Point2f& lhs, const cv::Point2f& rhs) {
        return lhs.x < rhs.x;
      });

  const auto left_top = raw[1].y > raw[0].y ? 0 : 1;
  const auto left_bottom = left_top == 0 ? 1 : 0;
  const auto right_top = raw[3].y > raw[2].y ? 2 : 3;
  const auto right_bottom = right_top == 2 ? 3 : 2;
  result.points = {
      raw[static_cast<std::size_t>(left_top)],
      raw[static_cast<std::size_t>(right_top)],
      raw[static_cast<std::size_t>(right_bottom)],
      raw[static_cast<std::size_t>(left_bottom)],
  };
  result.short_side =
      std::min(result.rectangle.size.width, result.rectangle.size.height);
  return result;
}

double polygon_area(const std::array<cv::Point2f, 4>& points) {
  double twice_area = 0.0;
  for (std::size_t index = 0; index < points.size(); ++index) {
    const auto& current = points[index];
    const auto& next = points[(index + 1) % points.size()];
    twice_area +=
        static_cast<double>(current.x) * next.y -
        static_cast<double>(next.x) * current.y;
  }
  return std::abs(twice_area) * 0.5;
}

double polygon_perimeter(const std::array<cv::Point2f, 4>& points) {
  double perimeter = 0.0;
  for (std::size_t index = 0; index < points.size(); ++index) {
    const auto& current = points[index];
    const auto& next = points[(index + 1) % points.size()];
    perimeter += cv::norm(current - next);
  }
  return perimeter;
}

float box_score_fast(
    const cv::Mat& probability,
    const std::array<cv::Point2f, 4>& points) {
  float min_x = points.front().x;
  float max_x = min_x;
  float min_y = points.front().y;
  float max_y = min_y;
  for (const auto& point : points) {
    min_x = std::min(min_x, point.x);
    max_x = std::max(max_x, point.x);
    min_y = std::min(min_y, point.y);
    max_y = std::max(max_y, point.y);
  }

  const auto xmin = std::clamp(
      static_cast<int>(std::floor(min_x)), 0, probability.cols - 1);
  const auto xmax = std::clamp(
      static_cast<int>(std::ceil(max_x)), 0, probability.cols - 1);
  const auto ymin = std::clamp(
      static_cast<int>(std::floor(min_y)), 0, probability.rows - 1);
  const auto ymax = std::clamp(
      static_cast<int>(std::ceil(max_y)), 0, probability.rows - 1);

  cv::Mat mask = cv::Mat::zeros(
      ymax - ymin + 1, xmax - xmin + 1, CV_8UC1);
  std::array<cv::Point, 4> local{};
  for (std::size_t index = 0; index < points.size(); ++index) {
    local[index] = cv::Point{
        static_cast<int>(points[index].x - xmin),
        static_cast<int>(points[index].y - ymin),
    };
  }
  const cv::Point* polygons[] = {local.data()};
  const int polygon_sizes[] = {static_cast<int>(local.size())};
  cv::fillPoly(mask, polygons, polygon_sizes, 1, cv::Scalar(1));
  return static_cast<float>(
      cv::mean(
          probability(cv::Rect{
              xmin, ymin, xmax - xmin + 1, ymax - ymin + 1}),
          mask)[0]);
}

float box_score_slow(
    const cv::Mat& probability,
    const std::vector<cv::Point>& contour) {
  const auto rect = cv::boundingRect(contour) &
                    cv::Rect{0, 0, probability.cols, probability.rows};
  if (rect.empty()) {
    return 0.0F;
  }
  cv::Mat mask = cv::Mat::zeros(rect.height, rect.width, CV_8UC1);
  std::vector<cv::Point> local;
  local.reserve(contour.size());
  for (const auto& point : contour) {
    local.push_back(point - rect.tl());
  }
  const std::vector<std::vector<cv::Point>> polygons{std::move(local)};
  cv::fillPoly(mask, polygons, cv::Scalar(1));
  return static_cast<float>(cv::mean(probability(rect), mask)[0]);
}

MiniBox unclip_rectangle(const MiniBox& box, float unclip_ratio) {
  const double perimeter = polygon_perimeter(box.points);
  if (perimeter <= 1e-9) {
    return box;
  }
  const double distance =
      polygon_area(box.points) * unclip_ratio / perimeter;
  if (distance <= 0.0) {
    return box;
  }

  // RapidOCR sends the float32 minAreaRect vertices to pyclipper. Its Python
  // binding truncates them to signed integers, then Clipper 6 applies a round
  // join with the default 0.25 arc tolerance. A direct float rectangle
  // expansion looks equivalent geometrically, but can move the final mapped
  // quad by one pixel. That is enough to push the direction classifier across
  // its strict 0.9 rotation threshold, so reproduce Clipper's integer/arc
  // contract before fitting the second minAreaRect.
  struct IntPoint {
    std::int64_t x{0};
    std::int64_t y{0};
  };
  struct UnitNormal {
    double x{0.0};
    double y{0.0};
  };
  const auto clipper_round = [](double value) {
    return static_cast<std::int64_t>(
        value < 0.0 ? value - 0.5 : value + 0.5);
  };
  std::array<IntPoint, 4> integer_points{};
  for (std::size_t index = 0; index < box.points.size(); ++index) {
    integer_points[index] = IntPoint{
        .x = static_cast<std::int64_t>(box.points[index].x),
        .y = static_cast<std::int64_t>(box.points[index].y),
    };
  }
  std::array<UnitNormal, 4> normals{};
  for (std::size_t index = 0; index < integer_points.size(); ++index) {
    const auto& current = integer_points[index];
    const auto& next = integer_points[(index + 1) % integer_points.size()];
    const double dx = static_cast<double>(next.x - current.x);
    const double dy = static_cast<double>(next.y - current.y);
    const double length = std::hypot(dx, dy);
    if (length <= 1e-9) {
      return box;
    }
    normals[index] = UnitNormal{.x = dy / length, .y = -dx / length};
  }

  constexpr double kPi = 3.1415926535897932384626433832795;
  constexpr double kTwoPi = 2.0 * kPi;
  constexpr double kArcTolerance = 0.25;
  const double tolerance =
      std::min(kArcTolerance, distance * kArcTolerance);
  const double steps =
      kPi / std::acos(1.0 - tolerance / distance);
  const double sine = std::sin(kTwoPi / steps);
  const double cosine = std::cos(kTwoPi / steps);
  const double steps_per_radian = steps / kTwoPi;

  std::vector<cv::Point> expanded_points;
  expanded_points.reserve(
      static_cast<std::size_t>(std::ceil(steps)) + box.points.size());
  std::size_t previous = integer_points.size() - 1;
  for (std::size_t current = 0;
       current < integer_points.size();
       ++current) {
    const auto& previous_normal = normals[previous];
    const auto& current_normal = normals[current];
    double sin_angle =
        previous_normal.x * current_normal.y -
        current_normal.x * previous_normal.y;
    sin_angle = std::clamp(sin_angle, -1.0, 1.0);
    const double cos_angle =
        current_normal.x * previous_normal.x +
        current_normal.y * previous_normal.y;
    const double angle = std::atan2(sin_angle, cos_angle);
    const auto arc_steps = std::max<std::int64_t>(
        1, clipper_round(steps_per_radian * std::abs(angle)));

    double normal_x = previous_normal.x;
    double normal_y = previous_normal.y;
    for (std::int64_t step = 0; step < arc_steps; ++step) {
      expanded_points.emplace_back(
          static_cast<int>(clipper_round(
              integer_points[current].x + normal_x * distance)),
          static_cast<int>(clipper_round(
              integer_points[current].y + normal_y * distance)));
      const double old_x = normal_x;
      normal_x = old_x * cosine - sine * normal_y;
      normal_y = old_x * sine + normal_y * cosine;
    }
    expanded_points.emplace_back(
        static_cast<int>(clipper_round(
            integer_points[current].x + current_normal.x * distance)),
        static_cast<int>(clipper_round(
            integer_points[current].y + current_normal.y * distance)));
    previous = current;
  }
  return get_mini_box(expanded_points);
}

std::int32_t round_coordinate(float value) {
  const double lower = std::floor(value);
  const double fraction = static_cast<double>(value) - lower;
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

std::array<cv::Point2f, 4> order_points_clockwise(
    const std::array<cv::Point2f, 4>& input) {
  auto x_sorted = input;
  std::stable_sort(
      x_sorted.begin(), x_sorted.end(),
      [](const cv::Point2f& lhs, const cv::Point2f& rhs) {
        return lhs.x < rhs.x;
      });
  std::array<cv::Point2f, 2> left{x_sorted[0], x_sorted[1]};
  std::array<cv::Point2f, 2> right{x_sorted[2], x_sorted[3]};
  std::stable_sort(
      left.begin(), left.end(),
      [](const cv::Point2f& lhs, const cv::Point2f& rhs) {
        return lhs.y < rhs.y;
      });
  std::stable_sort(
      right.begin(), right.end(),
      [](const cv::Point2f& lhs, const cv::Point2f& rhs) {
        return lhs.y < rhs.y;
      });
  return {left[0], right[0], right[1], left[1]};
}

QuadPolygon to_quad(
    std::array<cv::Point2f, 4> points,
    float score,
    int width,
    int height) {
  points = order_points_clockwise(points);
  for (auto& point : points) {
    point.x = static_cast<float>(std::clamp(
        static_cast<int>(point.x), 0, width - 1));
    point.y = static_cast<float>(std::clamp(
        static_cast<int>(point.y), 0, height - 1));
  }
  return QuadPolygon{
      .p0 = Point2D{points[0].x, points[0].y},
      .p1 = Point2D{points[1].x, points[1].y},
      .p2 = Point2D{points[2].x, points[2].y},
      .p3 = Point2D{points[3].x, points[3].y},
      .score = score,
  };
}

bool usable_quad(const QuadPolygon& quad) {
  const auto width = static_cast<int>(std::hypot(
      quad.p0.x - quad.p1.x, quad.p0.y - quad.p1.y));
  const auto height = static_cast<int>(std::hypot(
      quad.p0.x - quad.p3.x, quad.p0.y - quad.p3.y));
  return width > 3 && height > 3;
}

void sort_boxes(std::vector<QuadPolygon>* quads) {
  std::vector<float> positional_scores;
  positional_scores.reserve(quads->size());
  for (const auto& quad : *quads) {
    positional_scores.push_back(quad.score);
  }
  std::stable_sort(
      quads->begin(), quads->end(),
      [](const QuadPolygon& lhs, const QuadPolygon& rhs) {
        return lhs.p0.y < rhs.p0.y;
      });
  auto group_begin = quads->begin();
  while (group_begin != quads->end()) {
    auto group_end = group_begin + 1;
    while (group_end != quads->end() &&
           group_end->p0.y - (group_end - 1)->p0.y < 10.0F) {
      ++group_end;
    }
    std::stable_sort(
        group_begin, group_end,
        [](const QuadPolygon& lhs, const QuadPolygon& rhs) {
          return lhs.p0.x < rhs.p0.x;
        });
    group_begin = group_end;
  }
  for (std::size_t index = 0; index < quads->size(); ++index) {
    (*quads)[index].score = positional_scores[index];
  }
}

#endif

}  // namespace

DBPostProcessResult db_postprocess(
    const float* prob_map_data,
    int map_h,
    int map_w,
    int orig_h,
    int orig_w,
    const DBPostProcessOptions& options) {
  DBPostProcessResult result;
  if (prob_map_data == nullptr || map_h <= 0 || map_w <= 0 ||
      orig_h <= 0 || orig_w <= 0) {
    return result;
  }
  if (options.score_mode != "fast" && options.score_mode != "slow") {
    throw std::invalid_argument("Paddle DB score_mode must be fast or slow");
  }

#if defined(SUBLIFT_USE_OPENCV_FOR_DB)
  cv::Mat probability(
      map_h, map_w, CV_32FC1, const_cast<float*>(prob_map_data));
  cv::Mat bitmap;
  cv::threshold(
      probability, bitmap, options.det_thresh, 255.0, cv::THRESH_BINARY);
  bitmap.convertTo(bitmap, CV_8UC1);
  if (options.use_dilation) {
    cv::dilate(
        bitmap, bitmap,
        cv::getStructuringElement(cv::MORPH_RECT, cv::Size(2, 2)));
  }

  std::vector<std::vector<cv::Point>> contours;
  cv::findContours(
      bitmap, contours, cv::RETR_LIST, cv::CHAIN_APPROX_SIMPLE);
  const auto contour_count = std::min(
      contours.size(),
      static_cast<std::size_t>(std::max(0, options.max_candidates)));
  result.quads.reserve(contour_count);

  for (std::size_t index = 0; index < contour_count; ++index) {
    const auto& contour = contours[index];
    if (contour.empty()) {
      continue;
    }
    const auto mini_box = get_mini_box(contour);
    if (mini_box.short_side < options.min_size) {
      continue;
    }
    const float score =
        options.score_mode == "fast"
            ? box_score_fast(probability, mini_box.points)
            : box_score_slow(probability, contour);
    if (options.det_box_thresh > score) {
      continue;
    }

    const auto expanded = unclip_rectangle(mini_box, options.unclip_ratio);
    if (expanded.short_side < options.min_size + 2) {
      continue;
    }
    auto mapped = expanded.points;
    for (auto& point : mapped) {
      point.x = static_cast<float>(std::clamp(
          round_coordinate(point.x / map_w * orig_w), 0, orig_w));
      point.y = static_cast<float>(std::clamp(
          round_coordinate(point.y / map_h * orig_h), 0, orig_h));
    }
    const auto quad = to_quad(mapped, score, orig_w, orig_h);
    if (usable_quad(quad)) {
      result.quads.push_back(quad);
    }
  }

  result.aabbs.reserve(result.quads.size());
  for (const auto& quad : result.quads) {
    result.aabbs.push_back(
        quad_to_aabb({quad.p0, quad.p1, quad.p2, quad.p3}));
  }
#endif
  return result;
}

void sort_db_result(DBPostProcessResult* result) {
  if (result == nullptr) {
    return;
  }
#if defined(SUBLIFT_USE_OPENCV_FOR_DB)
  sort_boxes(&result->quads);
  result->aabbs.clear();
  result->aabbs.reserve(result->quads.size());
  for (const auto& quad : result->quads) {
    result->aabbs.push_back(
        quad_to_aabb({quad.p0, quad.p1, quad.p2, quad.p3}));
  }
#endif
}

}  // namespace sublift::paddle
