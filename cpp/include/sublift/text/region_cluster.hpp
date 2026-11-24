#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <sublift/core/box.hpp>
#include <sublift/core/models.hpp>

namespace sublift::text {

struct LineClusterOptions {
  std::string_view target_script{SCRIPT_AUTO};
  double min_confidence{0.28};
  double min_anchor_confidence{0.40};
  double min_script_score{0.20};
  double norm_y_min{0.55};
  double norm_y_max{0.98};
};

struct FrameClusterCandidate {
  bool detected{false};
  SourceBox selection{};            // Bounding box of detected subtitle text in full-frame video pixels
  double h_char_px{0.0};            // Character height in full-frame video pixels
  std::string preview_text{};       // Concatenated preview text of clustered lines
  double confidence{0.0};           // Average OCR confidence of clustered lines
  double cluster_score{0.0};        // Weighted cluster score
  std::int32_t total_candidates{0}; // Number of clustered lines

  friend bool operator==(const FrameClusterCandidate&, const FrameClusterCandidate&) = default;
};

/// Clusters OCR lines within a single frame into a candidate subtitle bounding box.
[[nodiscard]] FrameClusterCandidate cluster_frame_lines(
    std::span<const OcrLine> lines,
    std::int32_t frame_w, std::int32_t frame_h,
    std::int32_t orig_w, std::int32_t orig_h,
    const LineClusterOptions& options = {});

/// Merges cluster candidates across multiple sampled frames into a unified subtitle region.
[[nodiscard]] FrameClusterCandidate merge_cluster_candidates(
    std::span<const FrameClusterCandidate> candidates,
    std::int32_t orig_w, std::int32_t orig_h);

} // namespace sublift::text
