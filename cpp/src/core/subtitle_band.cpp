#include "sublift/core/subtitle_band.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace sublift {
namespace {

constexpr double kEps = 1e-12;

[[nodiscard]] double clamp01(double value) {
  return std::max(0.0, std::min(1.0, value));
}

[[nodiscard]] bool is_ascii_alnum(unsigned char c) {
  return std::isalnum(c) != 0;
}

[[nodiscard]] bool is_ascii_alpha(unsigned char c) {
  return std::isalpha(c) != 0;
}

[[nodiscard]] std::string ascii_lower(std::string_view text) {
  std::string out(text);
  for (char& c : out) {
    if (c >= 'A' && c <= 'Z') {
      c = static_cast<char>(c - 'A' + 'a');
    }
  }
  return out;
}

[[nodiscard]] bool is_known_tld(std::string_view tld) {
  static constexpr const char* kTlds[] = {
      "com", "net", "org", "cn",  "cc",  "tv", "me", "top", "vip", "xyz",
      "info", "io", "co",  "la",  "in",  "us", "uk", "jp",  "kr"};
  for (const char* candidate : kTlds) {
    if (tld == candidate) {
      return true;
    }
  }
  return false;
}

[[nodiscard]] double center_y(const SubtitleBandLine& line) {
  return line.y + line.height / 2.0;
}

[[nodiscard]] double center_x(const SubtitleBandLine& line) {
  return line.x + line.width / 2.0;
}

[[nodiscard]] bool valid_line(const SubtitleBandLine& line,
                              const SubtitleBandConfig& config) {
  if (!(line.width > 0.0) || !(line.height > 0.0)) {
    return false;
  }
  if (line.confidence < config.min_confidence) {
    return false;
  }
  if (line.height < config.min_line_height ||
      line.height > config.max_line_height) {
    return false;
  }
  const double cy = center_y(line);
  if (cy < config.min_center_y || cy > config.max_center_y) {
    return false;
  }
  return true;
}

/// 行级“像字幕”的外观分（与 docs/design/subtitle-band-detection.md §2.1 一致）。
[[nodiscard]] double line_score(const SubtitleBandLine& line) {
  const double cx = center_x(line);
  const double cy = center_y(line);
  double score = 0.0;
  if (cx >= 0.33 && cx <= 0.67) {
    score += 1.0;
  } else if (cx >= 0.25 && cx <= 0.75) {
    score += 0.55;
  } else {
    score += 0.05;
  }
  if (line.width >= 0.15) {
    score += 1.0;
  } else if (line.width >= 0.08) {
    score += 0.55;
  } else {
    score += 0.1;
  }
  if (cy >= 0.55 && cy <= 0.96) {
    score += 1.0;
  } else if (cy >= 0.45) {
    score += 0.4;
  }
  score += 0.3 * clamp01(line.confidence);
  if (contains_watermark_domain(line.text)) {
    score -= 1.6;
  }
  if ((cx < 0.15 || cx > 0.85) && line.width < 0.20) {
    score -= 0.8;
  }
  return score;
}

/// 贴边小框（角落台标/水印的几何特征）。
[[nodiscard]] bool edge_small(const SubtitleBandLine& line,
                              const SubtitleBandConfig& config) {
  const double cx = center_x(line);
  return (cx < config.side_watermark_edge_center ||
          cx > 1.0 - config.side_watermark_edge_center) &&
         line.width < config.side_watermark_max_width;
}

/// 跨帧几何匹配：同一位置/字号的行视为同一“轨道”。
[[nodiscard]] bool geometry_match(const SubtitleBandLine& ref,
                                  const SubtitleBandLine& other) {
  if (std::abs(center_y(ref) - center_y(other)) > 0.6 * ref.height) {
    return false;
  }
  if (!(other.height >= 0.55 * ref.height &&
        other.height <= 1.7 * ref.height)) {
    return false;
  }
  return std::abs(center_x(ref) - center_x(other)) <= 0.22;
}

/// 计算一条行的跨帧支持度；可选输出几何匹配到的行下标。
[[nodiscard]] int support_frames(const std::vector<SubtitleBandLine>& lines,
                                 std::size_t index,
                                 std::vector<std::size_t>* matches) {
  std::set<int> frames;
  const SubtitleBandLine& ref = lines[index];
  for (std::size_t j = 0; j < lines.size(); ++j) {
    if (!geometry_match(ref, lines[j])) {
      continue;
    }
    frames.insert(lines[j].frame_index);
    if (matches != nullptr) {
      matches->push_back(j);
    }
  }
  return static_cast<int>(frames.size());
}

/// 间歇性因子：字幕是间歇出现的；每帧常驻的文本（水印/台标）降权，
/// 单帧孤证也降权。
[[nodiscard]] double intermittency_factor(int support, int sample_frames) {
  if (sample_frames <= 0) {
    return 1.0;
  }
  const double ratio =
      static_cast<double>(support) / static_cast<double>(sample_frames);
  if (ratio > 0.90) {
    return 0.45;
  }
  if (ratio < 0.25) {
    return 0.60;
  }
  return 1.0;
}

[[nodiscard]] std::size_t utf8_codepoint_count(std::string_view text) {
  std::size_t count = 0;
  for (unsigned char c : text) {
    if ((c & 0xC0) != 0x80) {
      ++count;
    }
  }
  return count;
}

/// preview_text 只用于 UI 提示，过滤掉水印碎片/过短行，避免把水印文本展示给用户。
[[nodiscard]] bool preview_worthy(const SubtitleBandLine& line) {
  if (contains_watermark_domain(line.text)) {
    return false;
  }
  if (line.width < 0.15 && utf8_codepoint_count(line.text) < 6) {
    return false;
  }
  return !line.text.empty();
}

[[nodiscard]] int sample_frame_count(
    const std::vector<SubtitleBandLine>& lines) {
  int frames = 0;
  for (const auto& line : lines) {
    frames = std::max(frames, line.frame_index + 1);
  }
  return frames;
}

void sort_and_dedupe_preview(std::vector<std::pair<double, std::string>>& items,
                             std::string& out) {
  std::sort(items.begin(), items.end(), [](const auto& lhs, const auto& rhs) {
    return lhs.first < rhs.first;
  });
  std::vector<std::string> seen;
  std::size_t emitted = 0;
  for (const auto& item : items) {
    if (item.second.empty()) {
      continue;
    }
    if (std::find(seen.begin(), seen.end(), item.second) != seen.end()) {
      continue;
    }
    seen.push_back(item.second);
    if (!out.empty()) {
      out += ' ';
    }
    out += item.second;
    if (++emitted >= 8) {
      break;
    }
  }
}

}  // namespace

bool contains_watermark_domain(std::string_view text) {
  const std::string lower = ascii_lower(text);
  if (lower.find("http://") != std::string::npos ||
      lower.find("https://") != std::string::npos ||
      lower.find("www.") != std::string::npos) {
    return true;
  }
  for (std::size_t dot = 0; dot < lower.size(); ++dot) {
    if (lower[dot] != '.') {
      continue;
    }
    std::size_t begin = dot;
    while (begin > 0) {
      const auto c = static_cast<unsigned char>(lower[begin - 1]);
      if (is_ascii_alnum(c) || c == '-') {
        --begin;
      } else {
        break;
      }
    }
    if (dot - begin < 2) {
      continue;
    }
    std::size_t end = dot + 1;
    while (end < lower.size() &&
           is_ascii_alpha(static_cast<unsigned char>(lower[end]))) {
      ++end;
    }
    const std::size_t tld_len = end - (dot + 1);
    if (tld_len < 2 || tld_len > 6) {
      continue;
    }
    if (is_known_tld(std::string_view(lower).substr(dot + 1, tld_len))) {
      return true;
    }
  }
  return false;
}

SubtitleBandResult detect_subtitle_band(const std::vector<SubtitleBandLine>& lines,
                                        const SubtitleBandConfig& config) {
  SubtitleBandResult result;
  result.diagnostics.total_lines = static_cast<int>(lines.size());
  const int derived_frames = sample_frame_count(lines);
  const int sample_frames = config.sample_frames_hint > 0
                                ? config.sample_frames_hint
                                : derived_frames;
  result.diagnostics.sample_frames = sample_frames;

  if (lines.empty()) {
    result.diagnostics.reason = "empty_input";
    return result;
  }

  // ---- 1) 锚点行：外观分 × 间歇性 ----
  std::size_t anchor = 0;
  bool has_anchor = false;
  double best_anchor_score = -1.0;
  int anchor_support = 0;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    const auto& line = lines[i];
    if (!valid_line(line, config)) {
      continue;
    }
    if (contains_watermark_domain(line.text)) {
      continue;
    }
    const double score = line_score(line);
    if (score < config.anchor_min_score) {
      continue;
    }
    const int support = support_frames(lines, i, nullptr);
    const double total = score * intermittency_factor(support, sample_frames);
    // 阈值作用在“外观分 × 间歇性”上：单帧孤证的水印/台标行（0.45 折减后）
    // 不应成为锚点；真实的单帧字幕（外观分约 3+）仍可通过。
    if (total < config.anchor_min_score) {
      continue;
    }
    if (!has_anchor || total > best_anchor_score + kEps) {
      has_anchor = true;
      best_anchor_score = total;
      anchor = i;
      anchor_support = support;
    }
  }
  if (!has_anchor) {
    result.diagnostics.reason = "no_subtitle_anchor";
    return result;
  }

  const double anchor_h = lines[anchor].height;
  const double anchor_cx = center_x(lines[anchor]);
  result.diagnostics.anchor_frame_index = lines[anchor].frame_index;
  result.diagnostics.anchor_center_y = center_y(lines[anchor]);
  result.diagnostics.anchor_line_height = anchor_h;
  result.diagnostics.anchor_support_frames = anchor_support;

  // ---- 2) 从锚点中心向外逐行扩散 ----
  std::vector<char> included(lines.size(), 0);
  included[anchor] = 1;
  bool changed = true;
  while (changed) {
    changed = false;
    double band_lo = 1.0;
    double band_hi = 0.0;
    for (std::size_t i = 0; i < lines.size(); ++i) {
      if (!included[i]) {
        continue;
      }
      band_lo = std::min(band_lo, lines[i].y);
      band_hi = std::max(band_hi, lines[i].y + lines[i].height);
    }
    const double max_gap =
        std::max(config.expand_min_gap, config.expand_max_gap_factor * anchor_h);
    for (std::size_t j = 0; j < lines.size(); ++j) {
      if (included[j]) {
        continue;
      }
      const auto& line = lines[j];
      if (!valid_line(line, config)) {
        continue;
      }
      if (contains_watermark_domain(line.text)) {
        continue;
      }
      if (line_score(line) < config.expand_min_score) {
        continue;
      }
      const double ratio = line.height / anchor_h;
      if (ratio < config.expand_height_ratio_min ||
          ratio > config.expand_height_ratio_max) {
        continue;
      }
      if (std::abs(center_x(line) - anchor_cx) >
          config.expand_max_center_offset) {
        continue;
      }
      const double line_lo = line.y;
      const double line_hi = line.y + line.height;
      const double gap = (line_lo >= band_hi)   ? line_lo - band_hi
                         : (band_lo >= line_hi) ? band_lo - line_hi
                                                : 0.0;
      if (gap > max_gap) {
        continue;
      }
      included[j] = 1;
      changed = true;
    }
  }

  // ---- 3) band 几何 + 自适应 padding ----
  double lo = 1.0;
  double hi = 0.0;
  double sub_x0 = 1.0;
  double sub_x1 = 0.0;
  double confidence_sum = 0.0;
  int included_count = 0;
  std::vector<std::pair<double, std::string>> preview_items;
  for (std::size_t i = 0; i < lines.size(); ++i) {
    if (!included[i]) {
      continue;
    }
    const auto& line = lines[i];
    lo = std::min(lo, line.y);
    hi = std::max(hi, line.y + line.height);
    sub_x0 = std::min(sub_x0, line.x);
    sub_x1 = std::max(sub_x1, line.x + line.width);
    confidence_sum += clamp01(line.confidence);
    ++included_count;
    if (preview_worthy(line)) {
      preview_items.emplace_back(line.y, line.text);
    }
  }

  const double pad_y = std::max(config.pad_y_min, config.pad_y_factor * anchor_h);
  double y0 = clamp01(lo - pad_y);
  double y1 = clamp01(hi + pad_y);
  if (y1 - y0 < config.min_band_height) {
    const double center = (y0 + y1) / 2.0;
    y0 = clamp01(center - config.min_band_height / 2.0);
    y1 = clamp01(center + config.min_band_height / 2.0);
    if (y1 - y0 < config.min_band_height) {
      if (y0 <= 0.0) {
        y1 = std::min(1.0, config.min_band_height);
      } else {
        y0 = std::max(0.0, 1.0 - config.min_band_height);
      }
    }
  }

  // ---- 4) X 策略：默认全宽；仅侧边常驻水印存在时收窄 ----
  double band_x = 0.0;
  double band_w = 1.0;
  bool side_trimmed = false;
  if (config.enable_side_watermark_trim && included_count > 0) {
    const double sub_w = sub_x1 - sub_x0;
    bool side_found = false;
    double side_x0 = 1.0;
    double side_x1 = 0.0;
    for (std::size_t j = 0; j < lines.size(); ++j) {
      if (included[j]) {
        continue;
      }
      const auto& line = lines[j];
      if (!valid_line(line, config)) {
        continue;
      }
      const bool side_like = contains_watermark_domain(line.text) ||
                             edge_small(line, config);
      if (!side_like) {
        continue;
      }
      std::vector<std::size_t> matches;
      const int support = support_frames(lines, j, &matches);
      const double ratio =
          sample_frames > 0 ? static_cast<double>(support) / sample_frames
                            : 1.0;
      if (ratio < config.side_watermark_min_support_ratio) {
        continue;
      }
      double x0 = line.x;
      double x1 = line.x + line.width;
      for (const auto match : matches) {
        x0 = std::min(x0, lines[match].x);
        x1 = std::max(x1, lines[match].x + lines[match].width);
      }
      if (x1 <= sub_x0 - config.side_watermark_min_gap ||
          x0 >= sub_x1 + config.side_watermark_min_gap) {
        side_found = true;
        side_x0 = std::min(side_x0, x0);
        side_x1 = std::max(side_x1, x1);
      }
    }

    if (side_found) {
      const double pad_x = std::max(config.pad_x_min, config.pad_x_factor * sub_w);
      double bx0 = clamp01(sub_x0 - pad_x);
      double bx1 = clamp01(sub_x1 + pad_x);
      if (bx1 - bx0 < config.side_trim_min_width) {
        const double center = (bx0 + bx1) / 2.0;
        bx0 = clamp01(center - config.side_trim_min_width / 2.0);
        bx1 = clamp01(center + config.side_trim_min_width / 2.0);
        if (bx1 - bx0 < config.side_trim_min_width) {
          if (bx0 <= 0.0) {
            bx1 = std::min(1.0, config.side_trim_min_width);
          } else {
            bx0 = std::max(0.0, 1.0 - config.side_trim_min_width);
          }
        }
      }
      const double overlap =
          std::min(bx1, side_x1) - std::max(bx0, side_x0);
      if (overlap <= config.side_trim_overlap_tolerance) {
        band_x = bx0;
        band_w = bx1 - bx0;
        side_trimmed = true;
      }
    }
  }

  // ---- 5) 诊断：被排除的水印/台标行 ----
  int excluded = 0;
  std::vector<std::string> excluded_texts;
  for (std::size_t j = 0; j < lines.size(); ++j) {
    if (included[j]) {
      continue;
    }
    const auto& line = lines[j];
    if (!valid_line(line, config)) {
      continue;
    }
    bool watermarkish = contains_watermark_domain(line.text);
    if (!watermarkish && edge_small(line, config)) {
      std::vector<std::size_t> matches;
      const int support = support_frames(lines, j, &matches);
      const double ratio =
          sample_frames > 0 ? static_cast<double>(support) / sample_frames
                            : 1.0;
      watermarkish = ratio >= config.side_watermark_min_support_ratio;
    }
    if (!watermarkish) {
      continue;
    }
    ++excluded;
    if (excluded_texts.size() < 8) {
      excluded_texts.push_back(line.text);
    }
  }

  result.detected = true;
  result.x = band_x;
  result.y = y0;
  result.width = band_w;
  result.height = std::max(0.0, y1 - y0);
  result.confidence =
      included_count > 0 ? confidence_sum / included_count : 0.0;
  result.total_candidates = included_count;
  sort_and_dedupe_preview(preview_items, result.preview_text);
  result.diagnostics.included_lines = included_count;
  result.diagnostics.excluded_watermark_lines = excluded;
  result.diagnostics.excluded_texts = std::move(excluded_texts);
  result.diagnostics.side_watermark_trimmed = side_trimmed;
  return result;
}

}  // namespace sublift
