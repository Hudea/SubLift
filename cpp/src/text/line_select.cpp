#include <sublift/text/line_select.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

#include <sublift/text/cleaning.hpp>

namespace sublift::text {

LineScore score_line(const OcrLine& line, const SubtitleProfile& profile) {
  const double s_script = script_score(line.text, profile.script);
  const double cy =
      static_cast<double>(line.box.y) + static_cast<double>(line.box.height) / 2.0;
  const double band_lo = static_cast<double>(profile.y_min);
  double band_hi = static_cast<double>(profile.y_max);
  if (!(profile.y_max > profile.y_min)) {
    band_hi = band_lo + 1.0;
  }
  const double band_h = std::max(1.0, band_hi - band_lo);
  double s_y = 0.0;
  if (band_lo <= cy && cy <= band_hi) {
    const double dist =
        std::abs(cy - static_cast<double>(profile.center_y)) / band_h;
    s_y = std::max(0.0, 1.0 - dist);
  } else {
    const double overflow =
        (cy < band_lo) ? (band_lo - cy) / band_h : (cy - band_hi) / band_h;
    s_y = std::max(0.0, 1.0 - overflow);
  }
  const double ph = std::max(1.0, static_cast<double>(profile.height));
  const double lh = std::max(1.0, static_cast<double>(line.box.height));
  const double ratio = std::min(lh, ph) / std::max(lh, ph);
  const double s_h = ratio;
  const double cx =
      static_cast<double>(line.box.x) + static_cast<double>(line.box.width) / 2.0;
  const double span =
      std::max(static_cast<double>(profile.center_x) * 2.0, 1.0);
  const double s_c =
      std::max(0.0, 1.0 - std::abs(cx - static_cast<double>(profile.center_x)) / span);
  const double conf =
      std::isnan(line.confidence) ? 0.0 : std::clamp(line.confidence, 0.0, 1.0);
  const double total = 0.40 * s_script + 0.30 * s_y + 0.15 * s_h + 0.10 * s_c +
                       0.05 * conf;
  return LineScore{total, s_script, s_y, s_h, s_c, conf};
}

LineScore score_line(
    const std::string& text,
    double confidence,
    const Box2i& box,
    int frame_width,
    int frame_height,
    const TextProfile& profile) {
  SubtitleProfile prof;
  prof.script = profile.script;
  const int fw = (frame_width > 0) ? frame_width : 1920;
  const int fh = (frame_height > 0) ? frame_height : 1080;
  prof.center_x = fw / 2;
  prof.center_y = static_cast<int>(fh * profile.expected_y_ratio);
  const int half_band = static_cast<int>(fh * profile.y_band_tolerance);
  prof.y_min = std::max(0, prof.center_y - half_band);
  prof.y_max = std::min(fh, prof.center_y + half_band);
  prof.height = (prof.y_max > prof.y_min) ? (prof.y_max - prof.y_min) / 3 : 36;
  if (prof.height <= 0) {
    prof.height = 36;
  }

  OcrCropBox obox{box.x, box.y, box.width, box.height};
  OcrLine line{.text = text, .confidence = confidence, .box = obox};
  return score_line(line, prof);
}

std::vector<OcrLine> bridge_line_boxes(std::span<const OcrLine> lines,
                                       const SubtitleProfile& profile,
                                       double max_gap_h_ratio) {
  if (lines.empty()) {
    return {};
  }

  // Step 1: Effective single-line character height h_char
  double h_char = static_cast<double>(profile.height);
  const double band_h = static_cast<double>(profile.y_max - profile.y_min);
  bool h_char_valid = false;
  if (h_char > 0.0) {
    if (band_h > 0.0 && h_char <= 0.8 * band_h) {
      h_char_valid = true;
    } else if (h_char < 60.0) {
      h_char_valid = true;
    }
  }

  if (!h_char_valid) {
    std::vector<double> heights;
    heights.reserve(lines.size());
    for (const auto& l : lines) {
      if (l.box.height > 0 && l.box.width > 0 &&
          !normalize_whitespace(l.text).empty()) {
        heights.push_back(static_cast<double>(l.box.height));
      }
    }
    if (!heights.empty()) {
      std::sort(heights.begin(), heights.end());
      h_char = heights[heights.size() / 2];
    } else {
      h_char = 36.0;
    }
  }

  if (h_char <= 0.0) {
    h_char = 36.0;
  }

  const double max_gap = max_gap_h_ratio * h_char;

  // Step 2: Vertical band filtering
  double band_lo = static_cast<double>(profile.y_min) - 0.5 * h_char;
  double band_hi = static_cast<double>(profile.y_max) + 0.5 * h_char;
  if (profile.y_max <= profile.y_min) {
    if (profile.center_y > 0) {
      band_lo = static_cast<double>(profile.center_y) - 1.5 * h_char;
      band_hi = static_cast<double>(profile.center_y) + 1.5 * h_char;
    } else {
      band_lo = -1e9;
      band_hi = 1e9;
    }
  }

  std::vector<const OcrLine*> valid_cands;
  valid_cands.reserve(lines.size());
  for (const auto& l : lines) {
    if (l.box.width <= 0 || l.box.height <= 0 ||
        normalize_whitespace(l.text).empty()) {
      continue;
    }
    const double cy =
        static_cast<double>(l.box.y) + static_cast<double>(l.box.height) / 2.0;
    if (cy >= band_lo && cy <= band_hi) {
      valid_cands.push_back(&l);
    }
  }
  if (valid_cands.empty()) {
    return {};
  }

  // Step 3: Center anchor search
  const double cx_target = static_cast<double>(profile.center_x);
  const double cy_target = (profile.center_y > 0)
                               ? static_cast<double>(profile.center_y)
                               : (profile.y_max > profile.y_min
                                      ? static_cast<double>(profile.y_min + profile.y_max) / 2.0
                                      : 0.0);
  const OcrLine* anchor = nullptr;
  double min_dist_to_center = 1e18;
  double min_box_center_dist = 1e18;
  double min_cy_dist = 1e18;
  double min_h_diff = 1e18;

  bool has_cjk_cand = false;
  if (profile.script == SCRIPT_CJK) {
    for (const auto* l : valid_cands) {
      if (cjk_ratio(l->text) > 0.0) {
        has_cjk_cand = true;
        break;
      }
    }
  }

  for (const auto* l : valid_cands) {
    if (has_cjk_cand && cjk_ratio(l->text) == 0.0) {
      continue;
    }
    const double x0 = static_cast<double>(l->box.x);
    const double x1 = x0 + static_cast<double>(l->box.width);
    double dist = 0.0;
    if (cx_target >= x0 && cx_target <= x1) {
      dist = 0.0;
    } else if (cx_target < x0) {
      dist = x0 - cx_target;
    } else {
      dist = cx_target - x1;
    }

    const double box_cx = (x0 + x1) / 2.0;
    const double box_center_dist = std::abs(box_cx - cx_target);

    const double cand_h = static_cast<double>(l->box.height);
    const double cy = static_cast<double>(l->box.y) + cand_h / 2.0;
    const double cy_dist = (profile.center_y > 0 || profile.y_max > profile.y_min)
                                ? std::abs(cy - cy_target)
                                : 0.0;
    const double h_diff = std::abs(cand_h - h_char);

    bool is_better = false;
    if (dist < min_dist_to_center - 1e-6) {
      is_better = true;
    } else if (std::abs(dist - min_dist_to_center) <= 1e-6) {
      if (box_center_dist < min_box_center_dist - 1e-6) {
        is_better = true;
      } else if (std::abs(box_center_dist - min_box_center_dist) <= 1e-6) {
        if (cy_dist < min_cy_dist - 1e-6) {
          is_better = true;
        } else if (std::abs(cy_dist - min_cy_dist) <= 1e-6) {
          if (h_diff < min_h_diff - 1e-6) {
            is_better = true;
          }
        }
      }
    }

    if (is_better) {
      min_dist_to_center = dist;
      min_box_center_dist = box_center_dist;
      min_cy_dist = cy_dist;
      min_h_diff = h_diff;
      anchor = l;
    }
  }

  const double max_anchor_dist = std::max(2.5 * h_char, cx_target * 0.35);
  if (anchor == nullptr || min_dist_to_center > max_anchor_dist) {
    return {};
  }

  // Step 4: Outward bridging from anchor
  std::vector<const OcrLine*> chain;
  chain.push_back(anchor);
  std::int32_t chain_left = anchor->box.x;
  std::int32_t chain_right = anchor->box.x + anchor->box.width;
  const double anchor_cy = static_cast<double>(anchor->box.y) +
                           static_cast<double>(anchor->box.height) / 2.0;

  const auto is_height_compatible = [&](double cand_h, double cand_cy) {
    if (std::min(cand_h, h_char) / std::max(cand_h, h_char) >= 0.70) {
      return true;
    }
    if (cand_h >= 0.35 * h_char && cand_h <= 1.5 * h_char &&
        std::abs(cand_cy - anchor_cy) <= 0.6 * h_char) {
      return true;
    }
    const double anchor_h = static_cast<double>(anchor->box.height);
    if (anchor_h < 0.70 * h_char &&
        cand_h >= 0.70 * h_char && cand_h <= 1.5 * h_char) {
      return true;
    }
    return false;
  };

  // 4a. Leftward expansion
  bool expanded = true;
  while (expanded) {
    expanded = false;
    const OcrLine* best_left = nullptr;
    double best_dist = 1e18;
    bool chain_has_cjk = false;
    for (const auto* m : chain) {
      if (cjk_ratio(m->text) > 0.0) {
        chain_has_cjk = true;
        break;
      }
    }
    for (const auto* cand : valid_cands) {
      if (std::find(chain.begin(), chain.end(), cand) != chain.end()) {
        continue;
      }
      if (cand->box.x >= chain_left) {
        continue;
      }
      const std::int32_t cand_right = cand->box.x + cand->box.width;
      const std::int32_t raw_gap = chain_left - cand_right;
      const double cand_cy = static_cast<double>(cand->box.y) +
                             static_cast<double>(cand->box.height) / 2.0;
      const double cy_diff = std::abs(cand_cy - anchor_cy);
      if (cy_diff > 0.6 * h_char) {
        continue;
      }
      const double max_overlap =
          (cy_diff <= 0.3 * h_char) ? (0.6 * h_char) : (0.25 * h_char);
      if (static_cast<double>(raw_gap) < -max_overlap) {
        continue;
      }
      const double gap = static_cast<double>(std::max(0, raw_gap));
      if (gap > max_gap) {
        continue;
      }
      if (chain_has_cjk && cjk_ratio(cand->text) == 0.0 &&
          latin_ratio(cand->text) > 0.5) {
        if (cand->text.size() > 6 ||
            raw_gap > static_cast<std::int32_t>(0.8 * h_char)) {
          continue;
        }
      }
      const double cand_h = static_cast<double>(cand->box.height);
      if (!is_height_compatible(cand_h, cand_cy)) {
        continue;
      }
      const double dist = std::abs(static_cast<double>(raw_gap));
      if (dist < best_dist) {
        best_dist = dist;
        best_left = cand;
      }
    }
    if (best_left != nullptr) {
      chain.push_back(best_left);
      chain_left = std::min(chain_left, best_left->box.x);
      expanded = true;
    }
  }

  // 4b. Rightward expansion
  expanded = true;
  while (expanded) {
    expanded = false;
    const OcrLine* best_right = nullptr;
    double best_dist = 1e18;
    bool chain_has_cjk = false;
    for (const auto* m : chain) {
      if (cjk_ratio(m->text) > 0.0) {
        chain_has_cjk = true;
        break;
      }
    }
    for (const auto* cand : valid_cands) {
      if (std::find(chain.begin(), chain.end(), cand) != chain.end()) {
        continue;
      }
      const std::int32_t cand_left = cand->box.x;
      const std::int32_t cand_right = cand->box.x + cand->box.width;
      if (cand_right <= chain_right) {
        continue;
      }
      const std::int32_t raw_gap = cand_left - chain_right;
      const double cand_cy = static_cast<double>(cand->box.y) +
                             static_cast<double>(cand->box.height) / 2.0;
      const double cy_diff = std::abs(cand_cy - anchor_cy);
      if (cy_diff > 0.6 * h_char) {
        continue;
      }
      const double max_overlap =
          (cy_diff <= 0.3 * h_char) ? (0.6 * h_char) : (0.25 * h_char);
      if (static_cast<double>(raw_gap) < -max_overlap) {
        continue;
      }
      const double gap = static_cast<double>(std::max(0, raw_gap));
      if (gap > max_gap) {
        continue;
      }
      if (chain_has_cjk && cjk_ratio(cand->text) == 0.0 &&
          latin_ratio(cand->text) > 0.5) {
        if (cand->text.size() > 6 ||
            raw_gap > static_cast<std::int32_t>(0.8 * h_char)) {
          continue;
        }
      }
      const double cand_h = static_cast<double>(cand->box.height);
      if (!is_height_compatible(cand_h, cand_cy)) {
        continue;
      }
      const double dist = std::abs(static_cast<double>(raw_gap));
      if (dist < best_dist) {
        best_dist = dist;
        best_right = cand;
      }
    }
    if (best_right != nullptr) {
      chain.push_back(best_right);
      chain_right =
          std::max(chain_right, best_right->box.x + best_right->box.width);
      expanded = true;
    }
  }

  // Step 5: Consolidate chain sorted left-to-right
  std::sort(chain.begin(), chain.end(),
            [](const OcrLine* a, const OcrLine* b) {
              if (a->box.x != b->box.x) {
                return a->box.x < b->box.x;
              }
              return a->box.y < b->box.y;
            });

  std::int32_t min_x = chain.front()->box.x;
  std::int32_t max_x = chain.front()->box.x + chain.front()->box.width;
  std::int32_t min_y = chain.front()->box.y;
  std::int32_t max_y = chain.front()->box.y + chain.front()->box.height;

  std::string merged_text;
  double total_conf_weight = 0.0;
  double conf_acc = 0.0;

  for (std::size_t i = 0; i < chain.size(); ++i) {
    min_x = std::min(min_x, chain[i]->box.x);
    max_x = std::max(max_x, chain[i]->box.x + chain[i]->box.width);
    min_y = std::min(min_y, chain[i]->box.y);
    max_y = std::max(max_y, chain[i]->box.y + chain[i]->box.height);

    const double weight =
        std::max(1.0, static_cast<double>(chain[i]->text.size()));
    const double line_conf =
        std::isnan(chain[i]->confidence) ? 0.0 : chain[i]->confidence;
    conf_acc += line_conf * weight;
    total_conf_weight += weight;

    if (i > 0) {
      const std::int32_t prev_right =
          chain[i - 1]->box.x + chain[i - 1]->box.width;
      const std::int32_t curr_left = chain[i]->box.x;
      const std::int32_t space_gap = curr_left - prev_right;
      if (space_gap > static_cast<std::int32_t>(0.25 * h_char) &&
          (latin_ratio(chain[i - 1]->text) > 0.0 ||
           latin_ratio(chain[i]->text) > 0.0)) {
        if (!merged_text.empty() && merged_text.back() != ' ' &&
            !chain[i]->text.empty() && chain[i]->text.front() != ' ') {
          merged_text.push_back(' ');
        }
      }
    }
    merged_text.append(chain[i]->text);
  }

  double final_conf = (chain.size() == 1)
                          ? chain.front()->confidence
                          : (total_conf_weight > 0.0 ? (conf_acc / total_conf_weight) : 0.0);
  if (std::isnan(final_conf)) {
    final_conf = 0.0;
  }
  OcrCropBox union_box{min_x, min_y, max_x - min_x, max_y - min_y};
  return {OcrLine{std::move(merged_text), final_conf, union_box}};
}

std::optional<OcrLine> select_line(const std::vector<OcrLine>& lines,
                                   const SubtitleProfile& profile,
                                   double min_score, double min_script,
                                   double wide_aspect_threshold) {
  if (lines.empty()) {
    return std::nullopt;
  }

  // Backward compatibility: uninitialized profile
  if (profile.center_x == 0 && profile.height == 0) {
    std::optional<OcrLine> best;
    double best_score = -1.0;
    for (const auto& line : lines) {
      if (normalize_whitespace(line.text).empty()) {
        continue;
      }
      const auto sc = score_line(line, profile);
      if (std::isnan(sc.total) || sc.total < min_score) {
        continue;
      }
      if (profile.script != SCRIPT_AUTO && sc.script < min_script) {
        continue;
      }
      if (sc.total > best_score) {
        best_score = sc.total;
        best = line;
      }
    }
    return best;
  }

  // Determine strip aspect ratio:
  const double strip_h =
      (profile.y_max > profile.y_min)
          ? static_cast<double>(profile.y_max - profile.y_min)
          : ((profile.center_y > 0) ? static_cast<double>(profile.center_y * 2)
                                    : static_cast<double>(profile.height));
  const double strip_w = static_cast<double>(profile.center_x * 2);
  const double aspect =
      (strip_h > 0.0 && strip_w > 0.0) ? (strip_w / strip_h) : 0.0;
  const bool is_wide_strip = (aspect > wide_aspect_threshold);
  const bool is_adaptive_strip =
      is_wide_strip ||
      (profile.y_max > profile.y_min &&
       (static_cast<double>(profile.y_max - profile.y_min) < strip_h ||
        (profile.height > 0 && static_cast<double>(profile.height) < strip_h)));

  // On non-adaptive strips (aspect <= wide_aspect_threshold and no vertical band), direct select
  if (!is_adaptive_strip) {
    std::optional<OcrLine> best;
    double best_score = -1.0;
    for (const auto& line : lines) {
      if (normalize_whitespace(line.text).empty()) {
        continue;
      }
      const auto sc = score_line(line, profile);
      if (std::isnan(sc.total) || sc.total < min_score) {
        continue;
      }
      if (profile.script != SCRIPT_AUTO && sc.script < min_script) {
        continue;
      }
      if (sc.total > best_score) {
        best_score = sc.total;
        best = line;
      }
    }
    return best;
  }

  // Center-axis outward bridging for adaptive strips
  auto bridged = bridge_line_boxes(lines, profile);
  if (bridged.empty()) {
    return std::nullopt;
  }

  const auto& candidates = bridged;

  // Ensure height comparison in score_line uses true character height H_char
  SubtitleProfile eff_profile = profile;
  double eff_h_char = static_cast<double>(profile.height);
  const double band_h = static_cast<double>(profile.y_max - profile.y_min);
  bool h_valid = false;
  if (eff_h_char > 0.0) {
    if (band_h > 0.0 && eff_h_char <= 0.8 * band_h) {
      h_valid = true;
    } else if (eff_h_char < 60.0) {
      h_valid = true;
    }
  }
  if (!h_valid) {
    std::vector<double> heights;
    heights.reserve(lines.size());
    for (const auto& l : lines) {
      if (l.box.height > 0 && l.box.width > 0 &&
          !normalize_whitespace(l.text).empty()) {
        heights.push_back(static_cast<double>(l.box.height));
      }
    }
    if (!heights.empty()) {
      std::sort(heights.begin(), heights.end());
      eff_h_char = heights[heights.size() / 2];
    } else {
      eff_h_char = 36.0;
    }
    if (eff_h_char > 0.0) {
      eff_profile.height = static_cast<std::int32_t>(std::round(eff_h_char));
    }
  }

  std::optional<OcrLine> best;
  double best_score = -1.0;
  for (const auto& line : candidates) {
    if (normalize_whitespace(line.text).empty()) {
      continue;
    }
    const auto sc = score_line(line, eff_profile);
    if (std::isnan(sc.total) || sc.total < min_score) {
      continue;
    }
    if (profile.script != SCRIPT_AUTO && sc.script < min_script) {
      continue;
    }
    if (sc.total > best_score) {
      best_score = sc.total;
      best = line;
    }
  }
  return best;
}

std::optional<ScoredOcrLine> select_best_line(
    std::span<const ScoredOcrLine> candidates,
    double min_score) {
  std::optional<ScoredOcrLine> best;
  double best_score = -1.0;
  for (const auto& cand : candidates) {
    if (!std::isnan(cand.score.total) && cand.score.total >= min_score &&
        cand.score.total > best_score) {
      best_score = cand.score.total;
      best = cand;
    }
  }
  return best;
}

} // namespace sublift::text
