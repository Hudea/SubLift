#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sublift/models.hpp"

namespace sublift {

struct LineScore {
  double total{0.0};
  double script{0.0};
  double y_band{0.0};
  double height{0.0};
  double center{0.0};
  double confidence{0.0};

  friend constexpr bool operator==(const LineScore&, const LineScore&) = default;
};

struct ConsensusResult {
  std::string text;
  double confidence{0.0};
  int support_votes{0};

  friend bool operator==(const ConsensusResult&, const ConsensusResult&) = default;
};

[[nodiscard]] std::string normalize_ocr_text(std::string_view text);
[[nodiscard]] std::string cleanup_subtitle_text(std::string_view text,
                                                std::string_view script = SCRIPT_CJK);

[[nodiscard]] double cjk_ratio(std::string_view text);
[[nodiscard]] double latin_ratio(std::string_view text);
[[nodiscard]] double script_score(std::string_view text, std::string_view script);

[[nodiscard]] LineScore score_line(const OcrLine& line, const SubtitleProfile& profile);

[[nodiscard]] std::optional<OcrLine> select_line(
    const std::vector<OcrLine>& lines, const SubtitleProfile& profile,
    double min_score = 0.28, double min_script = 0.12);

[[nodiscard]] int edit_distance(std::string_view a, std::string_view b);
[[nodiscard]] bool is_similar(std::string_view a, std::string_view b);

[[nodiscard]] ConsensusResult consensus_text(
    const std::vector<std::pair<std::string, double>>& samples,
    std::string_view script = SCRIPT_AUTO);

[[nodiscard]] bool should_accept_text(
    std::string_view text, double confidence, const SubtitleProfile& profile,
    double confidence_threshold, double low_conf_threshold, int support_votes,
    int min_stable_votes = 2);

}  // namespace sublift
