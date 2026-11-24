#include <sublift/text/consensus.hpp>

#include <algorithm>
#include <cmath>
#include <regex>
#include <tuple>
#include <utility>
#include <vector>

#include <sublift/text/cleaning.hpp>

namespace sublift::text {
namespace {

const std::regex kLeadingLatinEdge{R"(^[A-Za-z][A-Za-z0-9 .:_/\-]*)"};
const std::regex kTrailingLatinEdge{R"([A-Za-z][A-Za-z0-9 .:_/\-]*$)"};

[[nodiscard]] bool is_blank_text(std::string_view text) noexcept {
  std::size_t i = 0;
  char32_t cp = 0;
  while (decode_utf8(text, i, cp)) {
    if (!is_space_cp(cp)) {
      return false;
    }
  }
  return true;
}

[[nodiscard]] std::string consensus_key(std::string_view text,
                                        std::string_view script) {
  const auto normalized = normalize_whitespace(text);
  if (script != SCRIPT_CJK) {
    return normalized;
  }
  std::string cjk_only;
  std::size_t i = 0;
  while (i < normalized.size()) {
    char32_t cp = 0;
    const std::size_t b = i;
    if (!decode_utf8(normalized, i, cp)) {
      break;
    }
    if (is_cjk_cp(cp)) {
      cjk_only.append(normalized.substr(b, i - b));
    }
  }
  return cjk_only.empty() ? normalized : cjk_only;
}

[[nodiscard]] std::string edge_match(const std::string& text,
                                     const std::regex& pattern) {
  std::smatch m;
  if (std::regex_search(text, m, pattern)) {
    auto s = m.str(0);
    while (!s.empty() && s.front() == ' ') {
      s.erase(s.begin());
    }
    while (!s.empty() && s.back() == ' ') {
      s.pop_back();
    }
    return s;
  }
  return {};
}

[[nodiscard]] std::string trim_unstable_latin_edges(
    std::string text, const std::vector<std::string>& cluster_texts) {
  auto leading = edge_match(text, kLeadingLatinEdge);
  if (!leading.empty()) {
    std::vector<std::string> variants;
    for (const auto& c : cluster_texts) {
      variants.push_back(edge_match(c, kLeadingLatinEdge));
    }
    std::sort(variants.begin(), variants.end());
    variants.erase(std::unique(variants.begin(), variants.end()), variants.end());
    if (variants.size() > 1) {
      text = std::regex_replace(text, kLeadingLatinEdge, "",
                                std::regex_constants::format_first_only);
      text = normalize_whitespace(text);
    }
  }
  auto trailing = edge_match(text, kTrailingLatinEdge);
  if (!trailing.empty()) {
    std::vector<std::string> variants;
    for (const auto& c : cluster_texts) {
      variants.push_back(edge_match(c, kTrailingLatinEdge));
    }
    std::sort(variants.begin(), variants.end());
    variants.erase(std::unique(variants.begin(), variants.end()), variants.end());
    if (variants.size() > 1) {
      std::smatch m;
      if (std::regex_search(text, m, kTrailingLatinEdge)) {
        text.erase(static_cast<std::size_t>(m.position()),
                   static_cast<std::size_t>(m.length()));
        text = normalize_whitespace(text);
      }
    }
  }
  return text;
}

} // namespace

int edit_distance(std::string_view a, std::string_view b) {
  const auto ca = to_codepoints(a);
  const auto cb = to_codepoints(b);
  if (ca == cb) {
    return 0;
  }
  if (ca.empty()) {
    return static_cast<int>(cb.size());
  }
  if (cb.empty()) {
    return static_cast<int>(ca.size());
  }
  std::vector<int> prev(cb.size() + 1);
  for (std::size_t j = 0; j <= cb.size(); ++j) {
    prev[j] = static_cast<int>(j);
  }
  for (std::size_t i = 1; i <= ca.size(); ++i) {
    std::vector<int> cur(cb.size() + 1);
    cur[0] = static_cast<int>(i);
    for (std::size_t j = 1; j <= cb.size(); ++j) {
      const int ins = cur[j - 1] + 1;
      const int del = prev[j] + 1;
      const int sub = prev[j - 1] + (ca[i - 1] == cb[j - 1] ? 0 : 1);
      cur[j] = std::min({ins, del, sub});
    }
    prev = std::move(cur);
  }
  return prev[cb.size()];
}

bool is_similar(std::string_view a, std::string_view b) {
  if (a.empty() || b.empty()) {
    return a == b;
  }
  const auto ca = to_codepoints(a);
  const auto cb = to_codepoints(b);
  auto contains = [](const std::vector<char32_t>& hay,
                     const std::vector<char32_t>& needle) {
    if (needle.empty() || needle.size() > hay.size()) {
      return false;
    }
    for (std::size_t i = 0; i + needle.size() <= hay.size(); ++i) {
      if (std::equal(needle.begin(), needle.end(),
                     hay.begin() + static_cast<std::ptrdiff_t>(i))) {
        return true;
      }
    }
    return false;
  };
  if ((contains(ca, cb) || contains(cb, ca)) &&
      std::min(ca.size(), cb.size()) >= 4) {
    return true;
  }
  const int ed = edit_distance(a, b);
  const auto mx = static_cast<double>(std::max(ca.size(), cb.size()));
  const double threshold = (std::max(ca.size(), cb.size()) <= 4) ? 0.34 : 0.40;
  return static_cast<double>(ed) / mx <= threshold;
}

ConsensusResult consensus_text(
    const std::vector<std::pair<std::string, double>>& samples,
    std::string_view script) {
  struct Sample {
    int index;
    std::string text;
    std::string key;
    double conf;
  };
  std::vector<Sample> cleaned;
  for (std::size_t index = 0; index < samples.size(); ++index) {
    const auto norm = normalize_whitespace(samples[index].first);
    if (norm.empty()) {
      continue;
    }
    const double raw_conf = samples[index].second;
    const double conf =
        std::isnan(raw_conf) ? 0.0 : std::clamp(raw_conf, 0.0, 1.0);
    cleaned.push_back(Sample{static_cast<int>(index), norm,
                             consensus_key(norm, script), conf});
  }
  if (cleaned.empty()) {
    return ConsensusResult{"", 0.0, 0};
  }

  std::vector<std::vector<Sample>> neighborhoods;
  for (const auto& center : cleaned) {
    std::vector<Sample> neighborhood;
    for (const auto& sample : cleaned) {
      if (is_similar(sample.key, center.key)) {
        neighborhood.push_back(sample);
      }
    }
    neighborhoods.push_back(std::move(neighborhood));
  }

  auto cluster_rank = [](const std::vector<Sample>& cluster) {
    int total_cost = 0;
    for (const auto& left : cluster) {
      for (const auto& right : cluster) {
        total_cost += edit_distance(left.key, right.key);
      }
    }
    int min_idx = cluster.front().index;
    for (const auto& item : cluster) {
      min_idx = std::min(min_idx, item.index);
    }
    return std::make_tuple(-static_cast<int>(cluster.size()), total_cost, min_idx);
  };

  auto best_cluster = *std::min_element(
      neighborhoods.begin(), neighborhoods.end(),
      [&](const auto& a, const auto& b) { return cluster_rank(a) < cluster_rank(b); });
  const int votes = static_cast<int>(best_cluster.size());

  auto medoid = *std::min_element(
      best_cluster.begin(), best_cluster.end(),
      [&](const Sample& cand, const Sample& other) {
        int cost_key = 0;
        int cost_text = 0;
        int cost_key_o = 0;
        int cost_text_o = 0;
        for (const auto& x : best_cluster) {
          cost_key += edit_distance(cand.key, x.key);
          cost_text += edit_distance(cand.text, x.text);
          cost_key_o += edit_distance(other.key, x.key);
          cost_text_o += edit_distance(other.text, x.text);
        }
        return std::make_tuple(cost_key, cost_text, cand.index) <
               std::make_tuple(cost_key_o, cost_text_o, other.index);
      });

  std::string text = medoid.text;
  std::vector<std::string> cluster_texts;
  for (const auto& item : best_cluster) {
    cluster_texts.push_back(item.text);
  }
  std::vector<std::string> unique = cluster_texts;
  std::sort(unique.begin(), unique.end());
  unique.erase(std::unique(unique.begin(), unique.end()), unique.end());
  if (script == SCRIPT_CJK && unique.size() > 1) {
    text = trim_unstable_latin_edges(text, cluster_texts);
  }
  double confidence = 0.0;
  for (const auto& item : best_cluster) {
    confidence += (std::isnan(item.conf) ? 0.0 : item.conf);
  }
  confidence /= static_cast<double>(votes);
  if (std::isnan(confidence)) {
    confidence = 0.0;
  }
  return ConsensusResult{text, confidence, votes};
}

ConsensusResult consensus_text(
    std::span<const std::string> candidate_texts,
    std::span<const double> confidences) {
  std::vector<std::pair<std::string, double>> samples;
  samples.reserve(candidate_texts.size());
  for (std::size_t i = 0; i < candidate_texts.size(); ++i) {
    const double conf = (i < confidences.size()) ? confidences[i] : 1.0;
    samples.emplace_back(candidate_texts[i], conf);
  }
  return consensus_text(samples, SCRIPT_AUTO);
}

bool should_accept_text(std::string_view text, double confidence,
                        const SubtitleProfile& profile,
                        double confidence_threshold, double low_conf_threshold,
                        int support_votes, int min_stable_votes) {
  if (is_blank_text(text)) {
    return false;
  }
  const double s = script_score(text, profile.script);
  if (profile.script == SCRIPT_CJK && s < 0.08 && latin_ratio(text) > 0.5) {
    return false;
  }
  if (profile.script == SCRIPT_LATIN && s < 0.08 && cjk_ratio(text) > 0.5) {
    return false;
  }
  if (confidence >= confidence_threshold &&
      (profile.script == SCRIPT_AUTO || s >= 0.08)) {
    return true;
  }
  if (confidence >= low_conf_threshold && support_votes >= min_stable_votes &&
      s >= 0.20) {
    return true;
  }
  if (confidence >= std::max(low_conf_threshold, 0.35) && s >= 0.45 &&
      support_votes >= 1) {
    return true;
  }
  return confidence >= confidence_threshold && s >= 0.35;
}

bool should_early_terminate_consensus(
    const ConsensusResult& partial_consensus,
    std::string_view current_frame_text,
    double current_frame_conf,
    const SubtitleProfile& profile,
    double confidence_threshold,
    double low_conf_threshold,
    double min_script_score) {
  const bool mixed_script = (cjk_ratio(current_frame_text) > 0.0 && latin_ratio(current_frame_text) > 0.0);
  if (current_frame_conf >= confidence_threshold &&
      script_score(current_frame_text, profile.script) >= min_script_score &&
      !(profile.script == SCRIPT_CJK && mixed_script)) {
    return true;
  }
  if (partial_consensus.support_votes >= 2 && partial_consensus.confidence >= low_conf_threshold) {
    return true;
  }
  return false;
}

bool consensus_retry_needed(
    std::string_view text,
    double confidence,
    double confidence_threshold) {
  if (normalize_ocr_text(text).empty()) {
    return true;
  }
  return std::isnan(confidence) || confidence < confidence_threshold;
}

} // namespace sublift::text
