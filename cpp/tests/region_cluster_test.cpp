#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <cmath>
#include <limits>
#include <vector>

#include <sublift/core/models.hpp>
#include <sublift/text/line_select.hpp>
#include <sublift/text/region_cluster.hpp>

using namespace sublift;
using namespace sublift::text;

TEST_CASE("Single Frame Clustering - Standard CJK Subtitle", "[text][cluster]") {
  std::vector<OcrLine> lines = {
      OcrLine{
          .text = "欢迎来到动物城",
          .confidence = 0.95,
          .box = OcrCropBox{700, 880, 520, 40},
      },
  };

  LineClusterOptions opts{
      .target_script = SCRIPT_CJK,
      .norm_y_min = 0.55,
      .norm_y_max = 0.98,
  };

  auto res = cluster_frame_lines(lines, 1920, 1080, 1920, 1080, opts);
  REQUIRE(res.detected);
  CHECK(res.selection.x == 700);
  CHECK(res.selection.y == 880);
  CHECK(res.selection.width == 520);
  CHECK(res.selection.height == 40);
  CHECK(res.h_char_px == 40.0);
  CHECK(res.preview_text == "欢迎来到动物城");
  CHECK(res.total_candidates == 1);
  CHECK(res.confidence >= 0.90);
}

TEST_CASE("Single Frame Clustering - Rejection of Corner Watermarks", "[text][cluster]") {
  std::vector<OcrLine> lines = {
      // Top right corner watermark
      OcrLine{
          .text = "LOGO",
          .confidence = 0.92,
          .box = OcrCropBox{1750, 600, 120, 30},
      },
      // Bottom left corner logo
      OcrLine{
          .text = "CCTV",
          .confidence = 0.90,
          .box = OcrCropBox{50, 950, 100, 30},
      },
  };

  LineClusterOptions opts{
      .target_script = SCRIPT_CJK,
      .norm_y_min = 0.55,
      .norm_y_max = 0.98,
  };

  auto res = cluster_frame_lines(lines, 1920, 1080, 1920, 1080, opts);
  CHECK_FALSE(res.detected);
}

TEST_CASE("Single Frame Clustering - Anti-Noise Script Prioritization (Defect 3 Root-out)", "[text][cluster]") {
  // Scenario: Sharp high-confidence Latin logo + slightly lower confidence CJK subtitle
  std::vector<OcrLine> lines = {
      OcrLine{
          .text = "HBO HD",
          .confidence = 0.99,
          .box = OcrCropBox{850, 830, 220, 35},
      },
      OcrLine{
          .text = "我流血啦…我死啦",
          .confidence = 0.85,
          .box = OcrCropBox{700, 900, 520, 40},
      },
  };

  LineClusterOptions opts{
      .target_script = SCRIPT_CJK,
      .norm_y_min = 0.55,
      .norm_y_max = 0.98,
  };

  auto res = cluster_frame_lines(lines, 1920, 1080, 1920, 1080, opts);
  REQUIRE(res.detected);
  // Anchor must be CJK subtitle, and HBO HD must NOT corrupt anchor or envelope
  CHECK(res.preview_text == "我流血啦…我死啦");
  CHECK(res.selection.y == 900);
  CHECK(res.selection.height == 40);
  CHECK(res.total_candidates == 1);
}

TEST_CASE("Single Frame Clustering - Two-line Subtitle Merging", "[text][cluster]") {
  std::vector<OcrLine> lines = {
      OcrLine{
          .text = "第一行中文对话",
          .confidence = 0.93,
          .box = OcrCropBox{700, 870, 520, 38},
      },
      OcrLine{
          .text = "第二行补充对话",
          .confidence = 0.91,
          .box = OcrCropBox{720, 915, 480, 38},
      },
  };

  LineClusterOptions opts{
      .target_script = SCRIPT_CJK,
  };

  auto res = cluster_frame_lines(lines, 1920, 1080, 1920, 1080, opts);
  REQUIRE(res.detected);
  CHECK(res.total_candidates == 2);
  CHECK(res.selection.y == 870);
  CHECK(res.selection.height == (915 + 38 - 870)); // 83px
  CHECK(res.preview_text == "第一行中文对话 第二行补充对话");
}

TEST_CASE("Single Frame Clustering - Large Vertical Gap Rejection", "[text][cluster]") {
  std::vector<OcrLine> lines = {
      // Subtitle at y=900, h=40
      OcrLine{
          .text = "正文对白字幕",
          .confidence = 0.95,
          .box = OcrCropBox{700, 900, 500, 40},
      },
      // Distant title/credit at y=800, h=40 (gap = 900 - 840 = 60px > 1.2 * 40 = 48px)
      OcrLine{
          .text = "远距片尾字幕",
          .confidence = 0.92,
          .box = OcrCropBox{700, 800, 500, 40},
      },
  };

  LineClusterOptions opts{
      .target_script = SCRIPT_CJK,
  };

  auto res = cluster_frame_lines(lines, 1920, 1080, 1920, 1080, opts);
  REQUIRE(res.detected);
  // Only the main subtitle line should be clustered
  CHECK(res.selection.y == 900);
  CHECK(res.selection.height == 40);
  CHECK(res.total_candidates == 1);
  CHECK(res.preview_text == "正文对白字幕");
}

TEST_CASE("Multi-Frame Candidate Merging - Consistent Candidates", "[text][cluster]") {
  std::vector<FrameClusterCandidate> candidates = {
      FrameClusterCandidate{
          .detected = true,
          .selection = SourceBox{700, 880, 500, 42},
          .h_char_px = 42.0,
          .preview_text = "第1帧字幕",
          .confidence = 0.92,
          .cluster_score = 1.8,
          .total_candidates = 1,
      },
      FrameClusterCandidate{
          .detected = true,
          .selection = SourceBox{650, 882, 600, 40},
          .h_char_px = 40.0,
          .preview_text = "第2帧更宽的字幕文本",
          .confidence = 0.94,
          .cluster_score = 2.1,
          .total_candidates = 1,
      },
  };

  auto merged = merge_cluster_candidates(candidates, 1920, 1080);
  REQUIRE(merged.detected);
  CHECK(merged.selection.x == 650);
  CHECK(merged.selection.y == 880);
  CHECK(merged.selection.width == 600);
  CHECK(merged.selection.height == 42);
  CHECK(merged.total_candidates == 2);
}

TEST_CASE("Multi-Frame Candidate Merging - Center Alignment Rejection (Defect 3 Root-out)", "[text][cluster]") {
  std::vector<FrameClusterCandidate> candidates = {
      // Primary bottom subtitle cluster
      FrameClusterCandidate{
          .detected = true,
          .selection = SourceBox{700, 880, 500, 40}, // center_y = 900
          .h_char_px = 40.0,
          .preview_text = "底部对白字幕",
          .confidence = 0.95,
          .cluster_score = 2.0,
          .total_candidates = 1,
      },
      // Rogue center-screen text candidate
      FrameClusterCandidate{
          .detected = true,
          .selection = SourceBox{700, 500, 500, 40}, // center_y = 520 (diff = 380px >> 0.75 * 40 = 30px)
          .h_char_px = 40.0,
          .preview_text = "屏幕中央标题",
          .confidence = 0.90,
          .cluster_score = 1.5,
          .total_candidates = 1,
      },
  };

  auto merged = merge_cluster_candidates(candidates, 1920, 1080);
  REQUIRE(merged.detected);
  // Center screen candidate must be REJECTED!
  CHECK(merged.selection.y == 880);
  CHECK(merged.selection.height == 40);
  CHECK(merged.total_candidates == 1);
  CHECK(merged.preview_text == "底部对白字幕");
}

TEST_CASE("Coordinate Mapping Parity & F1=0 Eradication (Defect 1 & Defect 2 Root-out)", "[text][cluster]") {
  // Video resolution: 1920x1080
  // Selection detected at full-frame coordinates:
  const SourceBox subtitle_box{400, 860, 1120, 50};
  // Extraction crop strip with padding:
  const SourceBox region_box{0, 848, 1920, 87};

  // 1. Correct relative profile mapping via from_selection_in_video
  SubtitleProfile correct_profile =
      SubtitleProfile::from_selection_in_video(region_box, subtitle_box, SCRIPT_CJK);

  CHECK(correct_profile.script == "cjk");
  CHECK(correct_profile.y_min == 12); // 860 - 848
  CHECK(correct_profile.y_max == 62); // 12 + 50
  CHECK(correct_profile.center_y == 37);

  // Local OCR line emitted inside cropped strip (height=87)
  std::vector<OcrLine> local_lines = {
      OcrLine{
          .text = "这是一条位于裁剪条内的中文字幕",
          .confidence = 0.92,
          .box = OcrCropBox{400, 12, 1120, 50},
      },
  };

  auto selected = select_line(local_lines, correct_profile, 0.28, 0.12);
  REQUIRE(selected.has_value());
  CHECK(selected->text == "这是一条位于裁剪条内的中文字幕");

  // 2. Demonstrate legacy defect (unmapped global profile -> 100% rejection -> F1 = 0.000)
  SubtitleProfile unmapped_global_profile{
      .script = std::string{SCRIPT_AUTO}, // Defect 2: script lost
      .center_x = 960,
      .center_y = 885,                    // Defect 1: full-frame Y in local strip
      .height = 50,
      .y_min = 860,
      .y_max = 910,
  };

  auto rejected = select_line(local_lines, unmapped_global_profile, 0.28, 0.12);
  // In legacy SubLift, this yielded nullopt for EVERY frame, giving 0 cues and F1=0.000!
  CHECK_FALSE(rejected.has_value());
}

// =============================================================================
// Regression Test Cases: Milestone M7 Hardening (Iteration 2)
// =============================================================================

TEST_CASE("Single Frame Clustering - NaN and Infinite Confidence Resilience", "[text][cluster][regression]") {
  const double qnan = std::numeric_limits<double>::quiet_NaN();
  const double inf = std::numeric_limits<double>::infinity();

  SECTION("All lines contain NaN or infinite confidence") {
    std::vector<OcrLine> lines = {
        OcrLine{
            .text = "NaN置信度干扰行",
            .confidence = qnan,
            .box = OcrCropBox{700, 880, 500, 40},
        },
        OcrLine{
            .text = "负无穷置信度干扰行",
            .confidence = -inf,
            .box = OcrCropBox{700, 880, 500, 40},
        },
    };

    auto res = cluster_frame_lines(lines, 1920, 1080, 1920, 1080);
    CHECK_FALSE(res.detected);
  }

  SECTION("Valid subtitle line alongside NaN and infinite lines") {
    std::vector<OcrLine> lines = {
        OcrLine{
            .text = "CORRUPTED_NAN_LINE",
            .confidence = qnan,
            .box = OcrCropBox{700, 880, 500, 40},
        },
        OcrLine{
            .text = "CORRUPTED_INF_LINE",
            .confidence = -inf,
            .box = OcrCropBox{700, 880, 500, 40},
        },
        OcrLine{
            .text = "有效中文字幕行",
            .confidence = 0.95,
            .box = OcrCropBox{700, 880, 500, 40},
        },
    };

    auto res = cluster_frame_lines(lines, 1920, 1080, 1920, 1080);
    REQUIRE(res.detected);
    // Preview text must contain only the valid line, never poisoned by NaN line
    CHECK(res.preview_text == "有效中文字幕行");
    CHECK(res.total_candidates == 1);
    CHECK_FALSE(std::isnan(res.confidence));
    CHECK_FALSE(std::isnan(res.cluster_score));
    CHECK(res.confidence >= 0.90);
    CHECK(res.selection.x == 700);
    CHECK(res.selection.y == 880);
    CHECK(res.selection.width == 500);
    CHECK(res.selection.height == 40);
  }

  SECTION("NaN confidence line cannot act as anchor or merge into multi-line cluster") {
    std::vector<OcrLine> lines = {
        OcrLine{
            .text = "第一行有效字幕",
            .confidence = 0.92,
            .box = OcrCropBox{700, 870, 500, 38},
        },
        OcrLine{
            .text = "伪造相邻第二行",
            .confidence = qnan,
            .box = OcrCropBox{700, 915, 500, 38},
        },
    };

    auto res = cluster_frame_lines(lines, 1920, 1080, 1920, 1080);
    REQUIRE(res.detected);
    CHECK(res.total_candidates == 1);
    CHECK(res.preview_text == "第一行有效字幕");
    CHECK(res.selection.y == 870);
    CHECK(res.selection.height == 38);
    CHECK_FALSE(std::isnan(res.confidence));
    CHECK_FALSE(std::isnan(res.cluster_score));
  }
}

TEST_CASE("Multi-Frame Candidate Merging - Canvas Dimension Validation", "[text][cluster][regression]") {
  std::vector<FrameClusterCandidate> valid_pair = {
      FrameClusterCandidate{
          .detected = true,
          .selection = SourceBox{700, 880, 500, 40},
          .h_char_px = 40.0,
          .preview_text = "第1帧",
          .confidence = 0.92,
          .cluster_score = 2.0,
          .total_candidates = 1,
      },
      FrameClusterCandidate{
          .detected = true,
          .selection = SourceBox{710, 880, 500, 40},
          .h_char_px = 40.0,
          .preview_text = "第2帧",
          .confidence = 0.94,
          .cluster_score = 2.2,
          .total_candidates = 1,
      },
  };

  SECTION("Non-positive canvas dimensions with multiple candidates") {
    // orig_w <= 0
    CHECK_FALSE(merge_cluster_candidates(valid_pair, 0, 1080).detected);
    CHECK_FALSE(merge_cluster_candidates(valid_pair, -1920, 1080).detected);
    CHECK_FALSE(merge_cluster_candidates(valid_pair, -100, 1080).detected);

    // orig_h <= 0
    CHECK_FALSE(merge_cluster_candidates(valid_pair, 1920, 0).detected);
    CHECK_FALSE(merge_cluster_candidates(valid_pair, 1920, -1080).detected);
    CHECK_FALSE(merge_cluster_candidates(valid_pair, 1920, -50).detected);

    // both non-positive
    CHECK_FALSE(merge_cluster_candidates(valid_pair, 0, 0).detected);
    CHECK_FALSE(merge_cluster_candidates(valid_pair, -100, -100).detected);
  }

  SECTION("Non-positive canvas dimensions with single candidate") {
    std::vector<FrameClusterCandidate> single_valid = {valid_pair[0]};

    CHECK_FALSE(merge_cluster_candidates(single_valid, 0, 1080).detected);
    CHECK_FALSE(merge_cluster_candidates(single_valid, -1920, 1080).detected);
    CHECK_FALSE(merge_cluster_candidates(single_valid, 1920, 0).detected);
    CHECK_FALSE(merge_cluster_candidates(single_valid, 1920, -1080).detected);
    CHECK_FALSE(merge_cluster_candidates(single_valid, -500, -500).detected);
  }

  SECTION("Non-positive canvas dimensions with empty candidates") {
    CHECK_FALSE(merge_cluster_candidates({}, 0, 1080).detected);
    CHECK_FALSE(merge_cluster_candidates({}, -100, -100).detected);
  }
}

TEST_CASE("Multi-Frame Candidate Merging - Corrupted NaN Candidate Resilience", "[text][cluster][regression]") {
  const double qnan = std::numeric_limits<double>::quiet_NaN();

  SECTION("All candidates corrupted with NaN score or confidence") {
    std::vector<FrameClusterCandidate> candidates = {
        FrameClusterCandidate{
            .detected = true,
            .selection = SourceBox{700, 880, 500, 40},
            .h_char_px = 40.0,
            .preview_text = "NaN分数候选",
            .confidence = 0.95,
            .cluster_score = qnan,
            .total_candidates = 1,
        },
        FrameClusterCandidate{
            .detected = true,
            .selection = SourceBox{700, 880, 500, 40},
            .h_char_px = 40.0,
            .preview_text = "NaN置信度候选",
            .confidence = qnan,
            .cluster_score = 2.0,
            .total_candidates = 1,
        },
        FrameClusterCandidate{
            .detected = true,
            .selection = SourceBox{700, 880, 500, 40},
            .h_char_px = qnan,
            .preview_text = "NaN字高候选",
            .confidence = 0.95,
            .cluster_score = 2.0,
            .total_candidates = 1,
        },
    };

    auto merged = merge_cluster_candidates(candidates, 1920, 1080);
    CHECK_FALSE(merged.detected);
  }

  SECTION("Valid candidates merged cleanly alongside leading and trailing NaN candidates") {
    std::vector<FrameClusterCandidate> candidates = {
        // Leading corrupted candidate (would poison best-pointer in buggy code)
        FrameClusterCandidate{
            .detected = true,
            .selection = SourceBox{100, 100, 200, 20},
            .h_char_px = 20.0,
            .preview_text = "POISON_LEADER",
            .confidence = qnan,
            .cluster_score = qnan,
            .total_candidates = 1,
        },
        // Legitimate candidate 1
        FrameClusterCandidate{
            .detected = true,
            .selection = SourceBox{700, 880, 500, 40},
            .h_char_px = 40.0,
            .preview_text = "第1帧对白",
            .confidence = 0.92,
            .cluster_score = 2.4,
            .total_candidates = 1,
        },
        // Legitimate candidate 2
        FrameClusterCandidate{
            .detected = true,
            .selection = SourceBox{680, 880, 540, 40},
            .h_char_px = 40.0,
            .preview_text = "第2帧完整对白",
            .confidence = 0.96,
            .cluster_score = 2.8,
            .total_candidates = 1,
        },
        // Trailing corrupted candidate
        FrameClusterCandidate{
            .detected = true,
            .selection = SourceBox{700, 880, 500, 40},
            .h_char_px = 40.0,
            .preview_text = "POISON_TRAILER",
            .confidence = 0.95,
            .cluster_score = qnan,
            .total_candidates = 1,
        },
    };

    auto merged = merge_cluster_candidates(candidates, 1920, 1080);
    REQUIRE(merged.detected);
    // Best valid candidate should be cand[2] with score 2.8
    CHECK(merged.preview_text == "第2帧完整对白");
    CHECK(merged.total_candidates == 2);
    CHECK_FALSE(std::isnan(merged.confidence));
    CHECK_FALSE(std::isnan(merged.cluster_score));
    CHECK(merged.confidence >= 0.90);
    CHECK(merged.selection.x == 680);
    CHECK(merged.selection.y == 880);
    CHECK(merged.selection.width == 540);
    CHECK(merged.selection.height == 40);
  }
}

TEST_CASE("Single Frame Clustering - Auto Script Rejects Prop Digits and Hangul Credits while Keeping Unsorted Dual-Line CJK", "[text][cluster][regression]") {
  SECTION("Prop digits and Hangul credits rejected alongside CJK anchor in SCRIPT_AUTO") {
    std::vector<OcrLine> lines = {
        OcrLine{
            .text = "请各位观众拨打屏幕下方举报热线",
            .confidence = 0.94,
            .box = OcrCropBox{580, 850, 760, 42},
        },
        OcrLine{
            .text = "8112,",
            .confidence = 0.98,
            .box = OcrCropBox{880, 795, 160, 40},
        },
        OcrLine{
            .text = "제작 김시형",
            .confidence = 0.95,
            .box = OcrCropBox{780, 900, 360, 38},
        },
    };

    const LineClusterOptions opts{.target_script = SCRIPT_AUTO};
    const auto res = cluster_frame_lines(lines, 1920, 1080, 1920, 1080, opts);
    REQUIRE(res.detected);
    CHECK(res.total_candidates == 1);
    CHECK(res.selection.y == 850);
    CHECK(res.selection.height == 42);
    CHECK(res.preview_text == "请各位观众拨打屏幕下方举报热线");
  }

  SECTION("Order-independent fixed-point clustering across unsorted multi-line CJK") {
    // Bottom line is anchor (h=36, max_vertical_gap=43.2, max_band_h=115.2), middle line is
    // last in input vector, top line (h=24 >= 0.6*36) has gap 890-844=46 > 43.2 from bottom
    // anchor alone, whereas once middle joins (850-844=6 <= 43.2) total span 926-820=106 <= 115.2.
    std::vector<OcrLine> lines = {
        OcrLine{
            .text = "第一行较短对白",
            .confidence = 0.88,
            .box = OcrCropBox{740, 820, 440, 24},
        },
        OcrLine{
            .text = "第三行宽底行对白文本较长",
            .confidence = 0.96,
            .box = OcrCropBox{580, 890, 760, 36},
        },
        OcrLine{
            .text = "第二行承接对白",
            .confidence = 0.90,
            .box = OcrCropBox{720, 850, 480, 36},
        },
    };

    const LineClusterOptions opts{.target_script = SCRIPT_AUTO};
    const auto res = cluster_frame_lines(lines, 1920, 1080, 1920, 1080, opts);
    REQUIRE(res.detected);
    CHECK(res.total_candidates == 3);
    CHECK(res.selection.y == 820);
    CHECK(res.selection.height == 106);
  }
}

TEST_CASE("Multi-Frame Candidate Merging - Baseline-Anchored Single-Line and Dual-Line Union", "[text][cluster][regression]") {
  const FrameClusterCandidate single_line{
      .detected = true,
      .selection = SourceBox{700, 850, 520, 40},
      .h_char_px = 40.0,
      .preview_text = "单行底部台词",
      .confidence = 0.95,
      .cluster_score = 3.2,
      .total_candidates = 1,
  };
  const FrameClusterCandidate dual_line{
      .detected = true,
      .selection = SourceBox{640, 788, 640, 102},
      .h_char_px = 40.0,
      .preview_text = "双行上部台词 单行底部台词",
      .confidence = 0.93,
      .cluster_score = 2.4,
      .total_candidates = 2,
  };
  const FrameClusterCandidate rogue_mid{
      .detected = true,
      .selection = SourceBox{700, 520, 500, 40},
      .h_char_px = 40.0,
      .preview_text = "屏幕中部标题",
      .confidence = 0.92,
      .cluster_score = 2.5,
      .total_candidates = 1,
  };

  SECTION("Single-line best absorbs bottom-aligned dual-line and rejects rogue mid-screen") {
    const std::vector<FrameClusterCandidate> cands = {single_line, dual_line, rogue_mid};
    const auto merged = merge_cluster_candidates(cands, 1920, 1080);
    REQUIRE(merged.detected);
    CHECK(merged.selection == SourceBox{640, 788, 640, 102});
    CHECK(merged.total_candidates == 2);
    CHECK(merged.h_char_px == 40.0);
    CHECK(merged.preview_text == "双行上部台词 单行底部台词");
  }

  SECTION("Dual-line best absorbs bottom-aligned single-line and rejects rogue mid-screen") {
    FrameClusterCandidate high_dual = dual_line;
    high_dual.cluster_score = 4.1;
    FrameClusterCandidate lower_single = single_line;
    lower_single.cluster_score = 2.6;

    const std::vector<FrameClusterCandidate> cands = {lower_single, high_dual, rogue_mid};
    const auto merged = merge_cluster_candidates(cands, 1920, 1080);
    REQUIRE(merged.detected);
    CHECK(merged.selection == SourceBox{640, 788, 640, 102});
    CHECK(merged.total_candidates == 2);
    CHECK(merged.h_char_px == 40.0);
    CHECK(merged.preview_text == "双行上部台词 单行底部台词");
  }
}

