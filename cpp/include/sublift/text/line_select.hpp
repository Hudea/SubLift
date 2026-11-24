#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <sublift/core/box.hpp>
#include <sublift/core/models.hpp>

namespace sublift::text {

struct LineScore {
  double total{0.0};
  double script{0.0};
  double y_band{0.0};
  double height{0.0};
  double center{0.0};
  double confidence{0.0};

  friend constexpr bool operator==(const LineScore&, const LineScore&) = default;
};

struct TextProfile {
  std::string script{"auto"};
  double expected_y_ratio{0.85};
  double y_band_tolerance{0.15};
};

struct ScoredOcrLine {
  std::string text;
  double confidence{0.0};
  Box2i box{};
  LineScore score{};

  friend bool operator==(const ScoredOcrLine&, const ScoredOcrLine&) = default;
};

/// 5-weight score for an OCR candidate line given a subtitle profile
[[nodiscard]] LineScore score_line(const OcrLine& line, const SubtitleProfile& profile);

/// Compatibility overload for frame-dimension based scoring
[[nodiscard]] LineScore score_line(
    const std::string& text,
    double confidence,
    const Box2i& box,
    int frame_width,
    int frame_height,
    const TextProfile& profile = {});

/// Horizontal center-anchored multi-box bridging for wide subtitle strips
[[nodiscard]] std::vector<OcrLine> bridge_line_boxes(
    std::span<const OcrLine> lines,
    const SubtitleProfile& profile,
    double max_gap_h_ratio = 2.0);

/// Primary candidate line selection combining strip adaptation, bridging, and scoring
[[nodiscard]] std::optional<OcrLine> select_line(
    const std::vector<OcrLine>& lines,
    const SubtitleProfile& profile,
    double min_score = 0.28,
    double min_script = 0.12,
    double wide_aspect_threshold = 6.0);

/// Compatibility overload for picking best pre-scored line
[[nodiscard]] std::optional<ScoredOcrLine> select_best_line(
    std::span<const ScoredOcrLine> candidates,
    double min_score = 0.28);

} // namespace sublift::text
