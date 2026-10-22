#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace sublift {

/// 归一化画面坐标（0..1，原点左上）的多帧 OCR 行证据。
///
/// 由区域检测器在多个采样帧上收集；`frame_index` 用于计算跨帧支持度。
struct SubtitleBandLine {
  int frame_index{0};
  double x{0.0};
  double y{0.0};
  double width{0.0};
  double height{0.0};
  std::string text;
  double confidence{0.0};
};

/// 自适应字幕条带参数。默认值来自 Phase 21 D01 的 4 片源原型标定。
struct SubtitleBandConfig {
  /// 采样帧数提示；0 表示由输入行的最大 frame_index+1 推导。
  /// 区域检测器应传入实际采样帧数（尾部空帧不产生行，推导会少算）。
  int sample_frames_hint{0};
  double min_confidence{0.40};
  double min_line_height{0.008};
  double max_line_height{0.11};
  double min_center_y{0.50};
  double max_center_y{0.985};

  double anchor_min_score{1.0};
  double expand_min_score{0.9};
  double expand_max_center_offset{0.30};
  double expand_max_gap_factor{0.9};
  double expand_min_gap{0.012};
  double expand_height_ratio_min{0.5};
  double expand_height_ratio_max{1.8};

  double pad_y_factor{0.30};
  double pad_y_min{0.008};
  double pad_x_factor{0.15};
  double pad_x_min{0.015};
  double min_band_height{0.03};

  /// 侧边常驻水印存在时，是否允许把 X 从全宽收窄到字幕行范围。
  bool enable_side_watermark_trim{true};
  double side_watermark_min_support_ratio{0.9};
  double side_watermark_edge_center{0.22};
  double side_watermark_max_width{0.35};
  double side_watermark_min_gap{0.02};
  double side_trim_min_width{0.60};
  double side_trim_overlap_tolerance{0.01};
};

/// 条带检测的可解释诊断（用于测试、DEBUG 与 UI 提示）。
struct SubtitleBandDiagnostics {
  int total_lines{0};
  int sample_frames{0};
  int anchor_frame_index{-1};
  double anchor_center_y{0.0};
  double anchor_line_height{0.0};
  int anchor_support_frames{0};
  int included_lines{0};
  int excluded_watermark_lines{0};
  std::vector<std::string> excluded_texts;
  bool side_watermark_trimmed{false};
  std::string reason;
};

struct SubtitleBandResult {
  bool detected{false};
  double x{0.0};
  double y{0.70};
  double width{1.0};
  double height{0.25};
  std::string preview_text;
  double confidence{0.0};
  int total_candidates{0};
  SubtitleBandDiagnostics diagnostics;
};

/// 判断文本是否包含水印常见 URL/域名标记（http(s)://、www.、`token.tld`）。
///
/// 规则刻意保守：只匹配常见 TLD 且 token 至少 2 个 ASCII 字符，避免误判
/// 正常对白里的缩写/句点。Phase 21 D03 的行级水印规则复用此函数。
[[nodiscard]] bool contains_watermark_domain(std::string_view text);

/// 多帧居中式自适应字幕条带检测。
///
/// 输入为多帧 OCR 行（归一化坐标）；输出归一化 ROI。算法与阈值见
/// docs/design/subtitle-band-detection.md：
/// 1) 行级外观分 + 跨帧几何支持度；
/// 2) 以「外观分 × 间歇性」选字幕锚点行；
/// 3) 从锚点中心向外逐行扩散，自适应 padding 得到 band 高度；
/// 4) 默认 X 全宽，仅在检测到侧边常驻水印族时收窄，宁宽勿窄。
///
/// 无锚点（例如全片只有 URL/台标行）时返回 detected=false，reason 可解释；
/// 不会把水印框当字幕区返回。
[[nodiscard]] SubtitleBandResult detect_subtitle_band(
    const std::vector<SubtitleBandLine>& lines,
    const SubtitleBandConfig& config = {});

}  // namespace sublift
