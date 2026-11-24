#include <sublift/text/region_cluster.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

#include <sublift/text/cleaning.hpp>

namespace sublift::text {

namespace {

struct InternalCand {
  const OcrLine* line{nullptr};
  std::string norm_text{};
  double cjk_r{0.0};
  double latin_r{0.0};
  double s_score{0.0};
  double weight{0.0};
  bool centered{false};
  bool is_target{false};
};

} // namespace

FrameClusterCandidate cluster_frame_lines(
    std::span<const OcrLine> lines,
    std::int32_t frame_w, std::int32_t frame_h,
    std::int32_t orig_w, std::int32_t orig_h,
    const LineClusterOptions& options) {
  if (lines.empty() || frame_w <= 0 || frame_h <= 0 || orig_w <= 0 || orig_h <= 0) {
    return FrameClusterCandidate{.detected = false};
  }

  std::vector<InternalCand> candidates;
  candidates.reserve(lines.size());

  const bool target_script_active =
      (!options.target_script.empty() && options.target_script != SCRIPT_AUTO);

  bool has_target_anchor_cand = false;
  bool has_linguistic_anchor_cand = false;

  for (const auto& line : lines) {
    if (std::isnan(line.confidence) || line.confidence < options.min_confidence) {
      continue;
    }
    if (line.box.width <= 0 || line.box.height <= 0) {
      continue;
    }

    std::string norm_text = normalize_whitespace(line.text);
    if (norm_text.empty()) {
      continue;
    }

    const double norm_y = static_cast<double>(line.box.y) / frame_h;
    if (norm_y < options.norm_y_min || norm_y > options.norm_y_max) {
      continue;
    }

    const double norm_x = static_cast<double>(line.box.x) / frame_w;
    const double norm_w = static_cast<double>(line.box.width) / frame_w;

    // Corner watermark rejection: top/bottom lateral corners with small width
    if ((norm_x > 0.75 || norm_x + norm_w < 0.25) && norm_w < 0.20) {
      continue;
    }

    const double center_x = norm_x + norm_w / 2.0;
    const bool centered = std::abs(center_x - 0.5) <= 0.22;
    const double cjk_r = cjk_ratio(norm_text);
    const double latin_r = latin_ratio(norm_text);
    const double s_score = script_score(norm_text, options.target_script);
    const bool is_target = (s_score >= options.min_script_score);

    double weight = line.confidence * (centered ? 1.5 : 1.0) * (norm_w >= 0.15 ? 1.3 : 1.0);
    if (target_script_active) {
      weight *= is_target ? (1.0 + s_score) : 0.35;
    } else {
      weight *= (0.7 + 0.6 * s_score);
    }

    if (line.confidence >= options.min_anchor_confidence && (!target_script_active || is_target)) {
      has_target_anchor_cand = true;
    }
    if (line.confidence >= options.min_anchor_confidence && (cjk_r >= 0.25 || latin_r >= 0.25)) {
      has_linguistic_anchor_cand = true;
    }

    candidates.push_back(InternalCand{
        .line = &line,
        .norm_text = std::move(norm_text),
        .cjk_r = cjk_r,
        .latin_r = latin_r,
        .s_score = s_score,
        .weight = weight,
        .centered = centered,
        .is_target = is_target,
    });
  }

  if (candidates.empty()) {
    return FrameClusterCandidate{.detected = false};
  }

  // Anchor selection: prefer matching target script if available
  const InternalCand* anchor = nullptr;
  double best_anchor_weight = -1.0;

  for (const auto& c : candidates) {
    if (c.line->confidence < options.min_anchor_confidence) {
      continue;
    }
    if (has_target_anchor_cand && target_script_active && !c.is_target) {
      continue; // Exclude non-target script lines from anchor when target script candidates exist
    }
    if (!target_script_active && has_linguistic_anchor_cand &&
        c.cjk_r == 0.0 && c.latin_r == 0.0) {
      continue; // Exclude prop digits / stray Hangul credits when CJK/Latin candidates exist
    }
    if (c.weight > best_anchor_weight) {
      best_anchor_weight = c.weight;
      anchor = &c;
    }
  }

  if (!anchor) {
    return FrameClusterCandidate{.detected = false};
  }

  const double anchor_h = static_cast<double>(anchor->line->box.height);
  const double max_vertical_gap = anchor_h * 1.2;
  const double max_band_h = std::min(static_cast<double>(frame_h) * 0.16, anchor_h * 3.2);

  double cluster_x0 = static_cast<double>(anchor->line->box.x);
  double cluster_x1 = static_cast<double>(anchor->line->box.x + anchor->line->box.width);
  double cluster_y0 = static_cast<double>(anchor->line->box.y);
  double cluster_y1 = static_cast<double>(anchor->line->box.y + anchor->line->box.height);

  std::vector<const InternalCand*> clustered;
  clustered.push_back(anchor);

  const bool anchor_is_cjk_or_latin = (anchor->cjk_r >= 0.25 || anchor->latin_r >= 0.25);
  std::vector<bool> in_cluster(candidates.size(), false);
  for (std::size_t i = 0; i < candidates.size(); ++i) {
    if (&candidates[i] == anchor) {
      in_cluster[i] = true;
      break;
    }
  }

  // Order-independent fixed-point expansion so distant top/bottom lines bridge via middle lines
  bool expanded = true;
  while (expanded) {
    expanded = false;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
      if (in_cluster[i]) {
        continue;
      }
      const auto& c = candidates[i];
      // Height consistency check
      const double h_i = static_cast<double>(c.line->box.height);
      if (h_i < 0.60 * anchor_h || h_i > 1.55 * anchor_h) {
        continue;
      }
      // Script anti-noise check: non-target lines cannot expand a target script cluster
      if (target_script_active && has_target_anchor_cand && !c.is_target) {
        continue;
      }
      if (!target_script_active && anchor_is_cjk_or_latin &&
          c.cjk_r == 0.0 && c.latin_r == 0.0) {
        continue;
      }

      const double c_y0 = static_cast<double>(c.line->box.y);
      const double c_y1 = static_cast<double>(c.line->box.y + c.line->box.height);

      double gap = 0.0;
      if (c_y1 < cluster_y0) {
        gap = cluster_y0 - c_y1;
      } else if (c_y0 > cluster_y1) {
        gap = c_y0 - cluster_y1;
      }

      if (gap <= max_vertical_gap) {
        const double new_y0 = std::min(cluster_y0, c_y0);
        const double new_y1 = std::max(cluster_y1, c_y1);
        if ((new_y1 - new_y0) <= max_band_h) {
          cluster_y0 = new_y0;
          cluster_y1 = new_y1;
          cluster_x0 = std::min(cluster_x0, static_cast<double>(c.line->box.x));
          cluster_x1 = std::max(cluster_x1, static_cast<double>(c.line->box.x + c.line->box.width));
          clustered.push_back(&c);
          in_cluster[i] = true;
          expanded = true;
        }
      }
    }
  }

  // Sort clustered lines vertically
  std::sort(clustered.begin(), clustered.end(), [](const InternalCand* a, const InternalCand* b) {
    return a->line->box.y < b->line->box.y;
  });

  std::string preview_text;
  double total_conf = 0.0;
  double total_score = 0.0;
  for (std::size_t i = 0; i < clustered.size(); ++i) {
    if (i > 0) {
      preview_text += " ";
    }
    preview_text += clustered[i]->norm_text;
    total_conf += clustered[i]->line->confidence;
    total_score += clustered[i]->weight;
  }

  const double scale_x = static_cast<double>(orig_w) / static_cast<double>(frame_w);
  const double scale_y = static_cast<double>(orig_h) / static_cast<double>(frame_h);

  const std::int32_t sel_x = std::clamp<std::int32_t>(
      static_cast<std::int32_t>(std::round(cluster_x0 * scale_x)), 0, orig_w);
  const std::int32_t sel_y = std::clamp<std::int32_t>(
      static_cast<std::int32_t>(std::round(cluster_y0 * scale_y)), 0, orig_h);
  const std::int32_t sel_w = std::clamp<std::int32_t>(
      static_cast<std::int32_t>(std::round((cluster_x1 - cluster_x0) * scale_x)), 0, orig_w - sel_x);
  const std::int32_t sel_h = std::clamp<std::int32_t>(
      static_cast<std::int32_t>(std::round((cluster_y1 - cluster_y0) * scale_y)), 0, orig_h - sel_y);

  return FrameClusterCandidate{
      .detected = true,
      .selection = SourceBox{sel_x, sel_y, sel_w, sel_h},
      .h_char_px = anchor_h * scale_y,
      .preview_text = std::move(preview_text),
      .confidence = total_conf / clustered.size(),
      .cluster_score = total_score,
      .total_candidates = static_cast<std::int32_t>(clustered.size()),
  };
}

FrameClusterCandidate merge_cluster_candidates(
    std::span<const FrameClusterCandidate> candidates,
    std::int32_t orig_w, std::int32_t orig_h) {
  if (orig_w <= 0 || orig_h <= 0) {
    return FrameClusterCandidate{.detected = false};
  }

  std::vector<const FrameClusterCandidate*> valid;
  for (const auto& c : candidates) {
    if (c.detected && c.selection.width > 0 && c.selection.height > 0 &&
        !std::isnan(c.confidence) && !std::isnan(c.cluster_score) &&
        !std::isnan(c.h_char_px)) {
      valid.push_back(&c);
    }
  }

  if (valid.empty()) {
    return FrameClusterCandidate{.detected = false};
  }
  if (valid.size() == 1) {
    return *valid.front();
  }

  // Pick best candidate with maximum cluster score
  const FrameClusterCandidate* best = valid.front();
  for (const auto* c : valid) {
    if (c->cluster_score > best->cluster_score) {
      best = c;
    }
  }

  const double best_center_y =
      static_cast<double>(best->selection.y) + static_cast<double>(best->selection.height) / 2.0;
  const double anchor_h = std::max(1.0, best->h_char_px);

  std::int32_t union_x0 = best->selection.x;
  std::int32_t union_x1 = best->selection.x + best->selection.width;
  std::int32_t union_y0 = best->selection.y;
  std::int32_t union_y1 = best->selection.y + best->selection.height;

  std::size_t merged_count = 1;
  double total_conf = best->confidence;
  double total_score = best->cluster_score;
  double merged_h_char = best->h_char_px;
  std::int32_t max_lines_in_frame = best->total_candidates;
  std::string merged_preview = best->preview_text;

  const double best_per_line_score =
      best->cluster_score / static_cast<double>(std::max<std::int32_t>(1, best->total_candidates));
  const double score_floor = std::max(0.60, best_per_line_score * 0.35);

  for (const auto* vc : valid) {
    if (vc == best) {
      continue;
    }
    if (vc->cluster_score < score_floor || vc->confidence < 0.38) {
      continue;
    }

    // Font size consistency
    const double min_h = std::min(best->h_char_px, vc->h_char_px);
    const double max_h = std::max(best->h_char_px, vc->h_char_px);
    if (max_h > 0.0 && (min_h / max_h) < (1.0 / 1.6)) {
      continue;
    }

    // Vertical center alignment or baseline-anchored multi-line containment
    const double vc_center_y =
        static_cast<double>(vc->selection.y) + static_cast<double>(vc->selection.height) / 2.0;
    const bool center_aligned = (std::abs(vc_center_y - best_center_y) <= anchor_h * 0.75);

    const std::int32_t best_y0 = best->selection.y;
    const std::int32_t best_y1 = best->selection.y + best->selection.height;
    const std::int32_t vc_y0 = vc->selection.y;
    const std::int32_t vc_y1 = vc->selection.y + vc->selection.height;
    const double ov_y = static_cast<double>(
        std::max<std::int32_t>(0, std::min(best_y1, vc_y1) - std::max(best_y0, vc_y0)));
    const double min_sel_h =
        static_cast<double>(std::min(best->selection.height, vc->selection.height));
    const bool shared_baseline =
        (std::abs(static_cast<double>(vc_y1 - best_y1)) <= anchor_h * 0.55) ||
        (std::abs(static_cast<double>(vc_y0 - best_y0)) <= anchor_h * 0.55);
    const bool baseline_contained =
        (best->total_candidates >= 2 || vc->total_candidates >= 2) &&
        (best->total_candidates != vc->total_candidates) &&
        (min_sel_h > 0.0 && ov_y >= 0.60 * min_sel_h) &&
        shared_baseline;

    if (!center_aligned && !baseline_contained) {
      continue;
    }

    // Vertical gap check
    double gap = 0.0;
    if (vc->selection.y + vc->selection.height < union_y0) {
      gap = static_cast<double>(union_y0 - (vc->selection.y + vc->selection.height));
    } else if (vc->selection.y > union_y1) {
      gap = static_cast<double>(vc->selection.y - union_y1);
    }
    if (gap > anchor_h * 0.6) {
      continue;
    }

    // Span ceiling check
    const std::int32_t new_y0 = std::min(union_y0, vc->selection.y);
    const std::int32_t new_y1 = std::max(union_y1, vc->selection.y + vc->selection.height);
    const bool involves_multiline =
        (best->total_candidates >= 2 || vc->total_candidates >= 2);
    const double max_span = involves_multiline
        ? std::min(
              static_cast<double>(orig_h) * 0.18,
              std::max(anchor_h * 3.4,
                       static_cast<double>(std::max(best->selection.height, vc->selection.height)) * 1.25))
        : std::min(
              static_cast<double>(orig_h) * 0.16,
              std::max(anchor_h * 3.0, static_cast<double>(best->selection.height) * 1.6));
    if (static_cast<double>(new_y1 - new_y0) > max_span) {
      continue;
    }

    union_x0 = std::min(union_x0, vc->selection.x);
    union_x1 = std::max(union_x1, vc->selection.x + vc->selection.width);
    union_y0 = new_y0;
    union_y1 = new_y1;
    merged_count++;
    total_conf += vc->confidence;
    total_score += vc->cluster_score;
    if (baseline_contained && vc->h_char_px > 0.0) {
      merged_h_char = std::min(merged_h_char, vc->h_char_px);
    }
    if (vc->total_candidates > max_lines_in_frame) {
      max_lines_in_frame = vc->total_candidates;
      merged_preview = vc->preview_text;
    }
  }

  union_x0 = std::clamp(union_x0, 0, orig_w);
  union_y0 = std::clamp(union_y0, 0, orig_h);
  const std::int32_t union_w = std::clamp(union_x1 - union_x0, 0, orig_w - union_x0);
  const std::int32_t union_h = std::clamp(union_y1 - union_y0, 0, orig_h - union_y0);

  return FrameClusterCandidate{
      .detected = true,
      .selection = SourceBox{union_x0, union_y0, union_w, union_h},
      .h_char_px = merged_h_char,
      .preview_text = std::move(merged_preview),
      .confidence = total_conf / merged_count,
      .cluster_score = total_score,
      .total_candidates = static_cast<std::int32_t>(merged_count),
  };
}

} // namespace sublift::text
