#include "adaptive_line_cluster.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

namespace sublift::features::detail {

namespace {

struct TextChain {
  std::vector<const SubtitleComponent*> comps;
  int total_area{0};
  int total_h{0};
  int total_w{0};
  int x_min{0};
  int x_max{0};
};

std::vector<int> extract_candidate_baselines(
    const std::vector<const SubtitleComponent*>& corridor_comps,
    int h,
    int min_h,
    int tol_y,
    bool enforce_coupled) {
  std::vector<int> votes_copy(static_cast<std::size_t>(h + tol_y + 2), 0);
  for (const auto* c : corridor_comps) {
    if (c->h < min_h || (enforce_coupled && !c->is_coupled) ||
        c->y_bot < static_cast<int>(0.35 * h)) {
      continue;
    }
    const int y_min_v = std::max(0, c->y_bot - tol_y);
    const int y_max_v = std::min(h, c->y_bot + tol_y);
    for (int yb = y_min_v; yb <= y_max_v; ++yb) {
      votes_copy[static_cast<std::size_t>(yb)]++;
    }
  }

  std::vector<int> candidate_baselines;
  for (int line_idx = 0; line_idx < 2; ++line_idx) {
    int max_aligned = 0, first_best = -1, last_best = -1;
    for (int yb = 0; yb <= h; ++yb) {
      const int v = votes_copy[static_cast<std::size_t>(yb)];
      if (v > max_aligned) {
        max_aligned = v;
        first_best = yb;
        last_best = yb;
      } else if (v == max_aligned && max_aligned > 0 && yb == last_best + 1) {
        last_best = yb;
      }
    }
    if (max_aligned < 2 || first_best < 0) break;

    candidate_baselines.push_back((first_best + last_best) / 2);
    std::fill(votes_copy.begin(), votes_copy.end(), 0);
    for (const auto* c : corridor_comps) {
      if (c->h < min_h || (enforce_coupled && !c->is_coupled) ||
          c->y_bot < static_cast<int>(0.35 * h)) {
        continue;
      }
      bool covered = false;
      for (int b_y : candidate_baselines) {
        if (std::abs(c->y_bot - b_y) <= 2 * tol_y) {
          covered = true;
          break;
        }
      }
      if (covered) continue;
      const int y_min_v = std::max(0, c->y_bot - tol_y);
      const int y_max_v = std::min(h, c->y_bot + tol_y);
      for (int yb = y_min_v; yb <= y_max_v; ++yb) {
        votes_copy[static_cast<std::size_t>(yb)]++;
      }
    }
    for (int b_y : candidate_baselines) {
      const int supp_min = std::max(0, b_y - 2 * tol_y);
      const int supp_max = std::min(h, b_y + 2 * tol_y);
      for (int yb = supp_min; yb <= supp_max; ++yb) {
        votes_copy[static_cast<std::size_t>(yb)] = 0;
      }
    }
  }
  return candidate_baselines;
}

bool accept_and_expand_baseline_chains(
    const std::vector<SubtitleComponent>& all_comps,
    const std::vector<const SubtitleComponent*>& corridor_comps,
    const std::vector<int>& candidate_baselines,
    int w,
    int min_h,
    int min_char_h,
    int min_multi_area,
    int tol_y,
    bool enforce_coupled,
    std::vector<std::uint8_t>& valid_labels) {
  bool is_text = false;
  for (int baseline_y : candidate_baselines) {
    std::vector<const SubtitleComponent*> line_comps;
    for (const auto* c : corridor_comps) {
      if (c->h >= min_h && (!enforce_coupled || c->is_coupled) &&
          std::abs(c->y_bot - baseline_y) <= tol_y) {
        line_comps.push_back(c);
      }
    }
    if (line_comps.empty()) continue;

    std::sort(line_comps.begin(), line_comps.end(),
              [](const SubtitleComponent* a, const SubtitleComponent* b) { return a->x < b->x; });

    std::vector<TextChain> chains;
    for (const auto* c : line_comps) {
      if (chains.empty()) {
        chains.push_back({{c}, c->area, c->h, c->w, c->x, c->x + c->w});
        continue;
      }
      auto& cur = chains.back();
      const auto* prev = cur.comps.back();
      const int gap_x = c->x - (prev->x + prev->w);
      const int max_gap = std::max(20, static_cast<int>(1.8 * std::max(c->h, prev->h)));
      const bool height_ok = std::min(c->h, prev->h) >= static_cast<int>(0.45 * std::max(c->h, prev->h));
      if (gap_x <= max_gap && height_ok) {
        cur.comps.push_back(c);
        cur.total_area += c->area;
        cur.total_h += c->h;
        cur.total_w += c->w;
        cur.x_max = std::max(cur.x_max, c->x + c->w);
      } else {
        chains.push_back({{c}, c->area, c->h, c->w, c->x, c->x + c->w});
      }
    }

    for (const auto& chn : chains) {
      const int cnt = static_cast<int>(chn.comps.size());
      const double avg_h = static_cast<double>(chn.total_h) / cnt;
      const double avg_w = static_cast<double>(chn.total_w) / cnt;
      if (cnt >= 5 && avg_w <= 0.35 * avg_h) continue;
      if (!(chn.x_min <= static_cast<int>(0.72 * w) && chn.x_max >= static_cast<int>(0.28 * w))) {
        continue;
      }

      int coupled_cnt = 0;
      int min_hc = chn.comps[0]->h, max_hc = chn.comps[0]->h;
      int min_yt = chn.comps[0]->y, max_yt = chn.comps[0]->y;
      for (const auto* c : chn.comps) {
        if (c->is_coupled) coupled_cnt++;
        min_hc = std::min(min_hc, c->h);
        max_hc = std::max(max_hc, c->h);
        min_yt = std::min(min_yt, c->y);
        max_yt = std::max(max_yt, c->y);
      }

      bool accepted = false;
      if (coupled_cnt >= 2) {
        accepted = (cnt >= 2 && chn.total_area >= min_multi_area && avg_h >= min_char_h) ||
                   (cnt >= 4 && avg_h >= min_char_h);
      } else if (cnt >= 5 && avg_h >= min_char_h && (max_yt - min_yt) <= 2 * tol_y) {
        accepted = true;
      } else if (cnt >= 2 && cnt <= 4 && avg_h >= min_char_h && chn.total_area >= min_multi_area) {
        const int cx = (chn.x_min + chn.x_max) / 2;
        accepted = (cx >= static_cast<int>(0.28 * w) && cx <= static_cast<int>(0.72 * w)) &&
                   ((chn.x_max - chn.x_min) <= static_cast<int>(4.5 * avg_h * cnt)) &&
                   (min_hc >= static_cast<int>(0.65 * max_hc)) &&
                   ((max_yt - min_yt) <= tol_y + 2);
      }
      if (!accepted) continue;

      for (const auto* c : chn.comps) valid_labels[static_cast<std::size_t>(c->label)] = 1;
      int cluster_x_min = chn.x_min, cluster_x_max = chn.x_max;
      const int max_gap_x = std::max(24, static_cast<int>(2.5 * avg_h));
      bool expanded = true;
      while (expanded) {
        expanded = false;
        for (const auto& c : all_comps) {
          if ((enforce_coupled && !c.is_coupled) || valid_labels[static_cast<std::size_t>(c.label)] ||
              std::abs(c.y_bot - baseline_y) > tol_y) {
            continue;
          }
          const bool adjacent =
              (c.x + c.w < cluster_x_min) ? ((cluster_x_min - (c.x + c.w)) <= max_gap_x)
              : (c.x > cluster_x_max)     ? ((c.x - cluster_x_max) <= max_gap_x)
                                          : true;
          if (adjacent) {
            valid_labels[static_cast<std::size_t>(c.label)] = 1;
            cluster_x_min = std::min(cluster_x_min, c.x);
            cluster_x_max = std::max(cluster_x_max, c.x + c.w);
            expanded = true;
          }
        }
      }
      is_text = true;
    }
  }
  return is_text;
}

bool try_corridor_fallbacks(
    const std::vector<const SubtitleComponent*>& corridor_comps,
    int w,
    int h,
    double scale,
    int min_h,
    int min_multi_area,
    int tol_y,
    std::vector<std::uint8_t>& valid_labels,
    int& out_chars) {
  auto include_nearby_coupled = [&](const SubtitleComponent& anchor) {
    const int max_gap_x = std::max(24, static_cast<int>(2.5 * anchor.h));
    for (const auto* other : corridor_comps) {
      if (other->label != anchor.label && other->is_coupled) {
        const bool y_overlap = (other->y <= anchor.y_bot + tol_y && other->y_bot >= anchor.y - tol_y);
        const int gap_x = std::max(
            0, std::max(other->x - (anchor.x + anchor.w), anchor.x - (other->x + other->w)));
        if (y_overlap && gap_x <= max_gap_x) {
          valid_labels[static_cast<std::size_t>(other->label)] = 1;
        }
      }
    }
  };

  if (corridor_comps.size() == 1) {
    const auto& c = *corridor_comps[0];
    const int cx = c.x + c.w / 2;
    const double aspect = static_cast<double>(c.w) / std::max(1, c.h);
    if (cx >= static_cast<int>(0.28 * w) && cx <= static_cast<int>(0.72 * w) &&
        c.y_bot >= static_cast<int>(0.35 * h) &&
        (c.y_bot <= static_cast<int>(0.95 * h) || (c.is_coupled && c.peak_c >= 110.0)) &&
        c.w >= std::max(10, static_cast<int>(12 * scale)) &&
        c.h >= std::max(12, static_cast<int>(14 * scale)) &&
        aspect >= 0.35 && aspect <= 35.0 &&
        c.area >= std::max(80, static_cast<int>(120 * scale * scale)) &&
        (c.peak_c >= 40.0 || c.is_coupled)) {
      valid_labels[static_cast<std::size_t>(c.label)] = 1;
      out_chars = std::max(out_chars, 1);
      include_nearby_coupled(c);
      return true;
    }
  } else if (corridor_comps.size() > 1) {
    for (const auto* cp : corridor_comps) {
      const auto& c = *cp;
      if (c.h < min_h) continue;
      const int cx = c.x + c.w / 2;
      const double aspect = static_cast<double>(c.w) / std::max(1, c.h);
      if (cx >= static_cast<int>(0.28 * w) && cx <= static_cast<int>(0.72 * w) &&
          c.y_bot >= static_cast<int>(0.35 * h) &&
          (c.y_bot <= static_cast<int>(0.95 * h) || c.peak_c >= 110.0) &&
          c.h >= std::max(12, static_cast<int>(16 * scale)) &&
          c.w >= std::max(10, static_cast<int>(14 * scale)) &&
          aspect >= 0.35 && aspect <= 35.0 &&
          c.area >= std::max(80, static_cast<int>(140 * scale * scale)) &&
          c.is_coupled && c.peak_c >= 50.0) {
        valid_labels[static_cast<std::size_t>(c.label)] = 1;
        out_chars = std::max(out_chars, 1);
        include_nearby_coupled(c);
        return true;
      }
    }
  }

  // Horizontal stroke characters ('一 二', '三 二 一')
  std::vector<const SubtitleComponent*> h_comps;
  int h_total_area = 0, h_min_y = h, h_max_y = 0, h_min_x = w, h_max_x = 0;
  for (const auto* cp : corridor_comps) {
    const auto& c = *cp;
    const int cx = c.x + c.w / 2;
    if (cx >= static_cast<int>(0.38 * w) && cx <= static_cast<int>(0.62 * w) &&
        c.y_bot >= static_cast<int>(0.55 * h) &&
        c.h <= std::max(7, static_cast<int>(8 * scale)) &&
        c.w >= std::max(24, static_cast<int>(35 * scale)) &&
        c.is_coupled && c.peak_c >= 115.0) {
      h_comps.push_back(cp);
      h_total_area += c.area;
      h_min_y = std::min(h_min_y, c.y);
      h_max_y = std::max(h_max_y, c.y_bot);
      h_min_x = std::min(h_min_x, c.x);
      h_max_x = std::max(h_max_x, c.x + c.w);
    }
  }
  if (h_comps.size() >= 2 && (h_max_y - h_min_y) <= static_cast<int>(0.42 * h) &&
      (h_max_x - h_min_x) <= static_cast<int>(2.2 * h) && h_total_area >= min_multi_area) {
    for (const auto* cp : h_comps) valid_labels[static_cast<std::size_t>(cp->label)] = 1;
    out_chars = std::max(out_chars, static_cast<int>(h_comps.size()));
    return true;
  }
  return false;
}

} // namespace

void cluster_polarity_text_lines(
    const std::vector<SubtitleComponent>& comps,
    const cv::Mat& labels,
    int num_labels,
    int w,
    int h,
    cv::Mat& mask_out,
    int& out_chars,
    int& out_area) {
  mask_out = cv::Mat::zeros(h, w, CV_8UC1);
  out_chars = 0;
  out_area = 0;
  if (comps.empty()) return;

  std::vector<const SubtitleComponent*> corridor_comps;
  for (const auto& c : comps) {
    if (c.in_corridor) corridor_comps.push_back(&c);
  }
  if (corridor_comps.empty()) return;

  const double scale = h / 120.0;
  const int min_h = std::max(8, static_cast<int>(10 * scale));
  const int tol_y = std::max(6, static_cast<int>(8 * scale));
  const int min_multi_area = std::max(35, static_cast<int>(110 * scale * scale));
  const int min_char_h = std::max(8, static_cast<int>(12 * scale));

  int total_coupled = 0;
  for (const auto* c : corridor_comps) {
    if (c->h >= min_h && c->is_coupled) total_coupled++;
  }
  const bool enforce_coupled = (total_coupled >= 2);

  const auto baselines =
      extract_candidate_baselines(corridor_comps, h, min_h, tol_y, enforce_coupled);
  std::vector<std::uint8_t> valid_labels(static_cast<std::size_t>(num_labels), 0);

  bool is_text = accept_and_expand_baseline_chains(
      comps, corridor_comps, baselines, w, min_h, min_char_h, min_multi_area,
      tol_y, enforce_coupled, valid_labels);
  if (!is_text) {
    is_text = try_corridor_fallbacks(
        corridor_comps, w, h, scale, min_h, min_multi_area, tol_y, valid_labels, out_chars);
  }
  if (!is_text) return;

  for (int r = 0; r < h; ++r) {
    const int* lbl_row = labels.ptr<int>(r);
    std::uint8_t* msk_row = mask_out.ptr<std::uint8_t>(r);
    for (int col = 0; col < w; ++col) {
      const int l = lbl_row[col];
      if (l > 0 && valid_labels[static_cast<std::size_t>(l)]) {
        msk_row[col] = 255;
      }
    }
  }

  int char_count_from_comps = 0;
  for (const auto& c : comps) {
    if (valid_labels[static_cast<std::size_t>(c.label)]) {
      out_area += c.area;
      if (c.h >= static_cast<int>(12 * scale)) {
        char_count_from_comps++;
      }
    }
  }
  out_chars = std::max(out_chars, char_count_from_comps);
}

} // namespace sublift::features::detail
