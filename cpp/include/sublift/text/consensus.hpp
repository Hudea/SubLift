#pragma once

#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sublift/core/models.hpp>

namespace sublift::text {

struct ConsensusResult {
  std::string text;
  double confidence{0.0};
  int support_votes{0};

  friend bool operator==(const ConsensusResult&, const ConsensusResult&) = default;
};

/// Levenshtein edit distance over Unicode code points.
[[nodiscard]] int edit_distance(std::string_view a, std::string_view b);

/// Fuzzy similarity test (containment >= 4 code points, or edit distance ratio <= 0.34/0.40).
[[nodiscard]] bool is_similar(std::string_view a, std::string_view b);

/// Representative frame consensus arbitration by clustering and medoid selection.
[[nodiscard]] ConsensusResult consensus_text(
    const std::vector<std::pair<std::string, double>>& samples,
    std::string_view script = SCRIPT_AUTO);

/// Overload for span of candidate texts and confidences (smoke/mock support).
[[nodiscard]] ConsensusResult consensus_text(
    std::span<const std::string> candidate_texts,
    std::span<const double> confidences = {});

/// Gating predicate to accept candidate text for subtitle entry generation.
[[nodiscard]] bool should_accept_text(
    std::string_view text, double confidence, const SubtitleProfile& profile,
    double confidence_threshold = 0.50, double low_conf_threshold = 0.28,
    int support_votes = 1, int min_stable_votes = 2);

/// Pure decision predicate for early termination during multi-frame OCR consensus.
[[nodiscard]] bool should_early_terminate_consensus(
    const ConsensusResult& partial_consensus,
    std::string_view current_frame_text,
    double current_frame_conf,
    const SubtitleProfile& profile,
    double confidence_threshold = 0.50,
    double low_conf_threshold = 0.28,
    double min_script_score = 0.12);

/// Pure predicate to check if OCR output needs retry with fallback frames.
[[nodiscard]] bool consensus_retry_needed(
    std::string_view text,
    double confidence,
    double confidence_threshold = 0.50);

} // namespace sublift::text
