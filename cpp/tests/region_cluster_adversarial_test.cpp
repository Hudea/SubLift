#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <sublift/core/models.hpp>
#include <sublift/text/region_cluster.hpp>

using namespace sublift;
using namespace sublift::text;

TEST_CASE("Region cluster adversarial: empty and degenerate inputs",
          "[text][region_cluster][adversarial]") {
  {
    auto res = cluster_frame_lines({}, 1920, 1080, 1920, 1080);
    REQUIRE_FALSE(res.detected);
  }

  {
    const std::vector<OcrLine> lines = {
        OcrLine{.text = "测试文本", .confidence = 0.90, .box = OcrCropBox{100, 800, 400, 40}},
    };
    REQUIRE_FALSE(cluster_frame_lines(lines, 0, 1080, 1920, 1080).detected);
    REQUIRE_FALSE(cluster_frame_lines(lines, 1920, 0, 1920, 1080).detected);
    REQUIRE_FALSE(cluster_frame_lines(lines, -1920, 1080, 1920, 1080).detected);
    REQUIRE_FALSE(cluster_frame_lines(lines, 1920, -1080, 1920, 1080).detected);
    REQUIRE_FALSE(cluster_frame_lines(lines, 1920, 1080, 0, 1080).detected);
    REQUIRE_FALSE(cluster_frame_lines(lines, 1920, 1080, 1920, 0).detected);
    REQUIRE_FALSE(cluster_frame_lines(lines, 1920, 1080, -1920, 1080).detected);
    REQUIRE_FALSE(cluster_frame_lines(lines, 1920, 1080, 1920, -1080).detected);
  }

  {
    const std::vector<OcrLine> lines = {
        OcrLine{.text = "", .confidence = 0.95, .box = OcrCropBox{100, 800, 400, 40}},
        OcrLine{.text = "   ", .confidence = 0.95, .box = OcrCropBox{100, 800, 400, 40}},
        OcrLine{.text = "\t\r\n \t", .confidence = 0.95, .box = OcrCropBox{100, 800, 400, 40}},
    };
    REQUIRE_FALSE(cluster_frame_lines(lines, 1920, 1080, 1920, 1080).detected);
  }

  {
    const std::vector<OcrLine> lines = {
        OcrLine{.text = "零宽文本", .confidence = 0.95, .box = OcrCropBox{100, 800, 0, 40}},
        OcrLine{.text = "负宽文本", .confidence = 0.95, .box = OcrCropBox{100, 800, -100, 40}},
        OcrLine{.text = "零高文本", .confidence = 0.95, .box = OcrCropBox{100, 800, 400, 0}},
        OcrLine{.text = "负高文本", .confidence = 0.95, .box = OcrCropBox{100, 800, 400, -50}},
    };
    REQUIRE_FALSE(cluster_frame_lines(lines, 1920, 1080, 1920, 1080).detected);
  }

  {
    const std::vector<OcrLine> lines = {
        OcrLine{.text = "低分视频测试字幕", .confidence = 0.92, .box = OcrCropBox{60, 120, 200, 20}},
    };
    const auto res = cluster_frame_lines(lines, 320, 180, 3840, 2160);
    REQUIRE(res.detected);
    REQUIRE(res.selection.x == 60 * 12);
    REQUIRE(res.selection.y == 120 * 12);
    REQUIRE(res.selection.width == 200 * 12);
    REQUIRE(res.selection.height == 20 * 12);
    REQUIRE(res.h_char_px == 20.0 * 12);
    REQUIRE(res.selection.x + res.selection.width <= 3840);
    REQUIRE(res.selection.y + res.selection.height <= 2160);
  }
}

TEST_CASE("Region cluster adversarial: confidence bounds and NaN handling",
          "[text][region_cluster][adversarial]") {
  {
    const std::vector<OcrLine> lines = {
        OcrLine{.text = "零置信度行", .confidence = 0.0, .box = OcrCropBox{700, 880, 500, 40}},
        OcrLine{.text = "负置信度行", .confidence = -1.5, .box = OcrCropBox{700, 880, 500, 40}},
    };
    REQUIRE_FALSE(cluster_frame_lines(lines, 1920, 1080, 1920, 1080).detected);
  }

  {
    const std::vector<OcrLine> lines = {
        OcrLine{.text = "极低置信行", .confidence = 0.27999, .box = OcrCropBox{700, 880, 500, 40}},
    };
    REQUIRE_FALSE(cluster_frame_lines(lines, 1920, 1080, 1920, 1080).detected);
  }

  {
    const std::vector<OcrLine> lines = {
        OcrLine{.text = "无锚点候选行1", .confidence = 0.35, .box = OcrCropBox{700, 880, 500, 40}},
        OcrLine{.text = "无锚点候选行2", .confidence = 0.38, .box = OcrCropBox{700, 930, 500, 40}},
    };
    REQUIRE_FALSE(cluster_frame_lines(lines, 1920, 1080, 1920, 1080).detected);
  }

  {
    const double qnan = std::numeric_limits<double>::quiet_NaN();
    const double neg_inf = -std::numeric_limits<double>::infinity();

    std::vector<OcrLine> nan_lines = {
        OcrLine{.text = "仅NaN置信度行", .confidence = qnan, .box = OcrCropBox{700, 880, 500, 40}},
        OcrLine{.text = "负无穷置信度行", .confidence = neg_inf, .box = OcrCropBox{700, 880, 500, 40}},
    };
    REQUIRE_FALSE(cluster_frame_lines(nan_lines, 1920, 1080, 1920, 1080).detected);

    std::vector<OcrLine> lines = nan_lines;
    lines.push_back(OcrLine{.text = "有效伴随字幕", .confidence = 0.95, .box = OcrCropBox{700, 880, 500, 40}});
    const auto res2 = cluster_frame_lines(lines, 1920, 1080, 1920, 1080);
    REQUIRE(res2.detected);
    REQUIRE(res2.preview_text == "有效伴随字幕");
    REQUIRE_FALSE(std::isnan(res2.confidence));
  }

  {
    const std::vector<OcrLine> lines = {
        OcrLine{.text = "超大置信度行", .confidence = 1000.0, .box = OcrCropBox{700, 880, 500, 40}},
    };
    const auto res = cluster_frame_lines(lines, 1920, 1080, 1920, 1080);
    REQUIRE(res.detected);
    REQUIRE(res.selection.x == 700);
    REQUIRE(res.selection.y == 880);
  }
}

TEST_CASE("Region cluster adversarial: out-of-bounds coordinates and envelope fuzzing",
          "[text][region_cluster][adversarial]") {
  {
    const std::vector<OcrLine> lines = {
        OcrLine{.text = "部分出界中文字幕", .confidence = 0.90, .box = OcrCropBox{-200, 880, 800, 40}},
    };
    const auto res = cluster_frame_lines(lines, 1920, 1080, 1920, 1080);
    if (res.detected) {
      REQUIRE(res.selection.x >= 0);
      REQUIRE(res.selection.x + res.selection.width <= 1920);
    }
  }

  {
    const std::vector<OcrLine> lines = {
        OcrLine{.text = "右侧越界中文字幕", .confidence = 0.92, .box = OcrCropBox{1600, 880, 600, 40}},
    };
    const auto res = cluster_frame_lines(lines, 1920, 1080, 1920, 1080);
    if (res.detected) {
      REQUIRE(res.selection.x >= 0);
      REQUIRE(res.selection.x + res.selection.width <= 1920);
    }
  }

  {
    const std::vector<OcrLine> lines = {
        OcrLine{.text = "底端贴边中文字幕", .confidence = 0.92, .box = OcrCropBox{700, 1040, 500, 80}},
    };
    const auto res = cluster_frame_lines(lines, 1920, 1080, 1920, 1080);
    if (res.detected) {
      REQUIRE(res.selection.y >= 0);
      REQUIRE(res.selection.y + res.selection.height <= 1080);
    }
  }

  {
    const std::vector<OcrLine> lines = {
        OcrLine{.text = "顶部中文标语", .confidence = 0.95, .box = OcrCropBox{700, 200, 500, 40}},
        OcrLine{.text = "中部居中中文", .confidence = 0.95, .box = OcrCropBox{700, 500, 500, 40}},
        OcrLine{.text = "越界底边标语", .confidence = 0.95, .box = OcrCropBox{700, 1070, 500, 40}},
    };
    REQUIRE_FALSE(cluster_frame_lines(lines, 1920, 1080, 1920, 1080).detected);
  }

  {
    constexpr auto kMax = std::numeric_limits<std::int32_t>::max();
    constexpr auto kMin = std::numeric_limits<std::int32_t>::min();
    const std::vector<OcrLine> lines = {
        OcrLine{.text = "极值行1", .confidence = 0.90, .box = OcrCropBox{kMax / 2, kMax / 2, 500, 40}},
        OcrLine{.text = "极值行2", .confidence = 0.90, .box = OcrCropBox{kMin / 2, kMin / 2, 500, 40}},
    };
    REQUIRE_FALSE(cluster_frame_lines(lines, 1920, 1080, 1920, 1080).detected);
  }

  {
    std::mt19937 rng(42);
    std::uniform_int_distribution<std::int32_t> coord_dist(-2000, 5000);
    std::uniform_int_distribution<std::int32_t> dim_dist(-200, 2000);
    std::uniform_real_distribution<double> conf_dist(-0.5, 1.5);

    for (int iter = 0; iter < 1000; ++iter) {
      std::vector<OcrLine> lines;
      lines.reserve(5);
      for (int i = 0; i < 5; ++i) {
        lines.push_back(OcrLine{
            .text = "随机蒙特卡洛测试行",
            .confidence = conf_dist(rng),
            .box = OcrCropBox{coord_dist(rng), coord_dist(rng), dim_dist(rng), dim_dist(rng)},
        });
      }

      const auto res = cluster_frame_lines(lines, 1920, 1080, 1920, 1080);
      if (res.detected) {
        REQUIRE(res.selection.x >= 0);
        REQUIRE(res.selection.y >= 0);
        REQUIRE(res.selection.width >= 0);
        REQUIRE(res.selection.height >= 0);
        REQUIRE(res.selection.x + res.selection.width <= 1920);
        REQUIRE(res.selection.y + res.selection.height <= 1080);
        REQUIRE(res.h_char_px >= 0.0);
      }
    }
  }
}

TEST_CASE("Region cluster adversarial: dense Latin watermark flood and script isolation",
          "[text][region_cluster][adversarial]") {
  {
    std::vector<OcrLine> lines;
    lines.reserve(101);
    for (int i = 0; i < 25; ++i) {
      lines.push_back(OcrLine{.text = "HBO HD " + std::to_string(i), .confidence = 0.99, .box = OcrCropBox{1700, 600 + i * 15, 180, 30}});
      lines.push_back(OcrLine{.text = "DYTT8.NET " + std::to_string(i), .confidence = 0.98, .box = OcrCropBox{50, 600 + i * 15, 180, 30}});
      lines.push_back(OcrLine{.text = "WWW.SUBTITLES.COM " + std::to_string(i), .confidence = 0.97, .box = OcrCropBox{800, 750 + i * 10, 300, 25}});
      lines.push_back(OcrLine{.text = "BITRATE: 8500KBPS", .confidence = 0.99, .box = OcrCropBox{100, 850 + i * 8, 220, 25}});
    }

    lines.push_back(OcrLine{
        .text = "狐狸和兔子一起拯救了动物城",
        .confidence = 0.85,
        .box = OcrCropBox{650, 910, 620, 42},
    });

    const LineClusterOptions opts{.target_script = SCRIPT_CJK};
    const auto res = cluster_frame_lines(lines, 1920, 1080, 1920, 1080, opts);

    REQUIRE(res.detected);
    REQUIRE(res.preview_text == "狐狸和兔子一起拯救了动物城");
    REQUIRE(res.total_candidates == 1);
    REQUIRE(res.selection.x == 650);
    REQUIRE(res.selection.y == 910);
    REQUIRE(res.selection.width == 620);
    REQUIRE(res.selection.height == 42);
  }

  {
    const std::vector<OcrLine> lines = {
        OcrLine{.text = "我们必须抓紧时间", .confidence = 0.93, .box = OcrCropBox{700, 880, 520, 38}},
        OcrLine{.text = "We must hurry up now", .confidence = 0.91, .box = OcrCropBox{720, 922, 480, 32}},
    };

    {
      const LineClusterOptions opts_cjk{.target_script = SCRIPT_CJK};
      const auto res = cluster_frame_lines(lines, 1920, 1080, 1920, 1080, opts_cjk);
      REQUIRE(res.detected);
      REQUIRE(res.total_candidates == 1);
      REQUIRE(res.preview_text == "我们必须抓紧时间");
      REQUIRE(res.selection.height == 38);
    }

    {
      const LineClusterOptions opts_auto{.target_script = SCRIPT_AUTO};
      const auto res = cluster_frame_lines(lines, 1920, 1080, 1920, 1080, opts_auto);
      REQUIRE(res.detected);
      REQUIRE(res.total_candidates == 2);
      REQUIRE(res.selection.height == (922 + 32 - 880));
      REQUIRE(res.preview_text.find("We must hurry up") != std::string::npos);
    }
  }

  {
    const std::vector<OcrLine> frame1_lines = {
        OcrLine{.text = "DIRECTED BY BYRON HOWARD", .confidence = 0.95, .box = OcrCropBox{700, 900, 520, 40}},
    };
    const LineClusterOptions opts_cjk{.target_script = SCRIPT_CJK};
    const auto cand1 = cluster_frame_lines(frame1_lines, 1920, 1080, 1920, 1080, opts_cjk);

    const std::vector<OcrLine> frame2_lines = {
        OcrLine{.text = "警官朱迪向您报到", .confidence = 0.95, .box = OcrCropBox{700, 900, 520, 40}},
    };
    const auto cand2 = cluster_frame_lines(frame2_lines, 1920, 1080, 1920, 1080, opts_cjk);

    const std::vector<FrameClusterCandidate> candidates = {cand1, cand2};
    const auto merged = merge_cluster_candidates(candidates, 1920, 1080);

    REQUIRE(merged.detected);
    REQUIRE(cand2.cluster_score > cand1.cluster_score * 2.0);
    REQUIRE(merged.preview_text == "警官朱迪向您报到");
  }
}

TEST_CASE("Region cluster adversarial: multi-frame conflict merging and volume stress",
          "[text][region_cluster][adversarial]") {
  {
    REQUIRE_FALSE(merge_cluster_candidates({}, 1920, 1080).detected);

    const std::vector<FrameClusterCandidate> undetected = {
        FrameClusterCandidate{.detected = false},
        FrameClusterCandidate{.detected = false, .selection = SourceBox{0, 0, 100, 100}},
        FrameClusterCandidate{.detected = true, .selection = SourceBox{0, 0, 0, 0}},
    };
    REQUIRE_FALSE(merge_cluster_candidates(undetected, 1920, 1080).detected);

    const std::vector<FrameClusterCandidate> two_valid = {
        FrameClusterCandidate{.detected = true, .selection = SourceBox{100, 800, 400, 40}, .confidence = 0.9, .cluster_score = 1.0},
        FrameClusterCandidate{.detected = true, .selection = SourceBox{110, 800, 400, 40}, .confidence = 0.9, .cluster_score = 1.1},
    };
    const auto neg_w_res = merge_cluster_candidates(two_valid, -100, 1080);
    REQUIRE((!neg_w_res.detected || neg_w_res.selection.x >= 0));
  }

  {
    const std::vector<FrameClusterCandidate> candidates = {
        FrameClusterCandidate{
            .detected = true,
            .selection = SourceBox{700, 880, 500, 40},
            .h_char_px = 40.0,
            .preview_text = "主字幕第1帧",
            .confidence = 0.96,
            .cluster_score = 3.5,
            .total_candidates = 1,
        },
        FrameClusterCandidate{
            .detected = true,
            .selection = SourceBox{680, 881, 550, 40},
            .h_char_px = 40.0,
            .preview_text = "主字幕第2帧较长",
            .confidence = 0.94,
            .cluster_score = 3.3,
            .total_candidates = 1,
        },
        FrameClusterCandidate{
            .detected = true,
            .selection = SourceBox{700, 500, 500, 40},
            .h_char_px = 40.0,
            .preview_text = "屏幕正中突兀广告",
            .confidence = 0.95,
            .cluster_score = 3.4,
            .total_candidates = 1,
        },
        FrameClusterCandidate{
            .detected = true,
            .selection = SourceBox{700, 100, 500, 40},
            .h_char_px = 40.0,
            .preview_text = "顶部演职人员表",
            .confidence = 0.95,
            .cluster_score = 3.4,
            .total_candidates = 1,
        },
        FrameClusterCandidate{
            .detected = true,
            .selection = SourceBox{200, 1040, 1500, 30},
            .h_char_px = 30.0,
            .preview_text = "底部走字滚动新闻",
            .confidence = 0.95,
            .cluster_score = 3.4,
            .total_candidates = 1,
        },
    };

    const auto merged = merge_cluster_candidates(candidates, 1920, 1080);
    REQUIRE(merged.detected);
    REQUIRE(merged.total_candidates == 2);
    REQUIRE(merged.selection.x == 680);
    REQUIRE(merged.selection.y == 880);
    REQUIRE(merged.selection.width == 550);
    REQUIRE(merged.selection.height == 41);
  }

  {
    const std::vector<FrameClusterCandidate> candidates = {
        FrameClusterCandidate{
            .detected = true,
            .selection = SourceBox{700, 880, 500, 40},
            .h_char_px = 40.0,
            .preview_text = "正常字幕字号",
            .confidence = 0.95,
            .cluster_score = 3.5,
            .total_candidates = 1,
        },
        FrameClusterCandidate{
            .detected = true,
            .selection = SourceBox{700, 875, 500, 50},
            .h_char_px = 75.0,
            .preview_text = "巨大字号非对白",
            .confidence = 0.92,
            .cluster_score = 2.5,
            .total_candidates = 1,
        },
    };
    const auto merged = merge_cluster_candidates(candidates, 1920, 1080);
    REQUIRE(merged.total_candidates == 1);
  }

  {
    std::vector<FrameClusterCandidate> candidates;
    candidates.reserve(10);
    for (int i = 0; i < 10; ++i) {
      candidates.push_back(FrameClusterCandidate{
          .detected = true,
          .selection = SourceBox{700, 800 + i * 15, 500, 40},
          .h_char_px = 40.0,
          .preview_text = "连续重叠扩张帧 " + std::to_string(i),
          .confidence = 0.95,
          .cluster_score = 3.0 + i * 0.05,
          .total_candidates = 1,
      });
    }

    const auto merged = merge_cluster_candidates(candidates, 1920, 1080);
    REQUIRE(merged.detected);
    REQUIRE(merged.selection.height <= 120);
  }

  {
    constexpr int kCandidatesCount = 10000;
    std::vector<FrameClusterCandidate> candidates;
    candidates.reserve(kCandidatesCount);

    std::mt19937 rng(12345);
    std::uniform_int_distribution<std::int32_t> y_dist(0, 1040);
    std::uniform_int_distribution<std::int32_t> x_dist(0, 1500);
    std::uniform_int_distribution<std::int32_t> w_dist(100, 800);
    std::uniform_real_distribution<double> conf_dist(0.30, 0.99);

    for (int i = 0; i < kCandidatesCount; ++i) {
      candidates.push_back(FrameClusterCandidate{
          .detected = true,
          .selection = SourceBox{x_dist(rng), y_dist(rng), w_dist(rng), 40},
          .h_char_px = 40.0,
          .preview_text = "批量压力测试文本",
          .confidence = conf_dist(rng),
          .cluster_score = conf_dist(rng) * 3.0,
          .total_candidates = 1,
      });
    }

    const auto merged = merge_cluster_candidates(candidates, 1920, 1080);
    REQUIRE(merged.detected);
    REQUIRE(merged.selection.x >= 0);
    REQUIRE(merged.selection.y >= 0);
    REQUIRE(merged.selection.x + merged.selection.width <= 1920);
    REQUIRE(merged.selection.y + merged.selection.height <= 1080);

    std::vector<OcrLine> big_lines;
    big_lines.reserve(10000);
    for (int i = 0; i < 10000; ++i) {
      big_lines.push_back(OcrLine{
          .text = "压力测试对白行 " + std::to_string(i),
          .confidence = conf_dist(rng),
          .box = OcrCropBox{x_dist(rng), y_dist(rng), w_dist(rng), 40},
      });
    }

    const auto cluster_res = cluster_frame_lines(big_lines, 1920, 1080, 1920, 1080);
    REQUIRE(cluster_res.detected);
  }
}
