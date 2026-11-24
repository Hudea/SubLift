#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <sublift/core/models.hpp>
#include <sublift/text/cleaning.hpp>
#include <sublift/text/consensus.hpp>
#include <sublift/text/line_select.hpp>
#include <sublift/text/srt.hpp>

using namespace sublift;
using namespace sublift::text;

TEST_CASE("normalize_ocr_text collapses whitespace", "[line_select]") {
  REQUIRE(sublift::text::normalize_ocr_text("  你  好  ") == "你 好");
}

TEST_CASE("cleanup ellipsis and quotes", "[line_select]") {
  REQUIRE(sublift::text::cleanup_subtitle_text("你好...", "cjk") == "你好…");
  REQUIRE(sublift::text::cleanup_subtitle_text("（前情提要⋯）", "cjk") ==
          "（前情提要…）");
  REQUIRE(sublift::text::cleanup_subtitle_text("他们说：『没问题』", "cjk") ==
          "他们说：「没问题」");
  REQUIRE(sublift::text::cleanup_subtitle_text("事实上.", "cjk") == "事实上…");
}

TEST_CASE("cleanup strips only latin attached to CJK edges", "[line_select]") {
  REQUIRE(sublift::text::cleanup_subtitle_text("（前市长杨咩咩入狱）PHISON", "cjk") ==
          "（前市长杨咩咩入狱）");
  REQUIRE(sublift::text::cleanup_subtitle_text("在动物方城市气候墙SON", "cjk") ==
          "在动物方城市气候墙");
  REQUIRE(sublift::text::cleanup_subtitle_text("FOR一起揭穿阴谋SON", "cjk") ==
          "一起揭穿阴谋");
  REQUIRE(sublift::text::cleanup_subtitle_text(
              "哈茱蒂，本市第一位兔警员EFS CONSPI _N/、", "cjk") ==
          "哈茱蒂，本市第一位兔警员");
}

TEST_CASE("cleanup keeps legitimate mixed English", "[line_select]") {
  REQUIRE(sublift::text::cleanup_subtitle_text("HELLO", "cjk") == "HELLO");
  REQUIRE(sublift::text::cleanup_subtitle_text("欢迎来到 ZPD", "cjk") ==
          "欢迎来到 ZPD");
  REQUIRE(sublift::text::cleanup_subtitle_text("任务代号 FOX TWO", "cjk") ==
          "任务代号 FOX TWO");
  REQUIRE(sublift::text::cleanup_subtitle_text("后来加入ZPD警局", "cjk") ==
          "后来加入ZPD警局");
}

TEST_CASE("normalize unicode whitespace", "[line_select]") {
  // Ideographic space U+3000 and NBSP should collapse like Python \s
  const std::string ideo = "你\u3000好";
  const std::string nbsp = "你\u00a0好";
  REQUIRE(sublift::text::normalize_ocr_text(ideo) == "你 好");
  REQUIRE(sublift::text::normalize_ocr_text(nbsp) == "你 好");
}

TEST_CASE("edit_distance kitten/sitting", "[line_select]") {
  REQUIRE(sublift::text::edit_distance("kitten", "sitting") == 3);
}

TEST_CASE("select_line prefers CJK in band", "[line_select]") {
  sublift::SubtitleProfile profile;
  profile.script = "cjk";
  profile.center_x = 160;
  profile.center_y = 40;
  profile.height = 30;
  profile.y_min = 20;
  profile.y_max = 60;
  std::vector<sublift::OcrLine> lines{
      sublift::OcrLine{"BREAKING", 0.99, {10, 5, 100, 20}},
      sublift::OcrLine{"你好世界", 0.9, {100, 30, 120, 28}},
  };
  const auto sel = sublift::text::select_line(lines, profile);
  REQUIRE(sel.has_value());
  REQUIRE(sel->text == "你好世界");
}

TEST_CASE("consensus_text cjk majority", "[line_select]") {
  const auto r = sublift::text::consensus_text(
      {{"你好", 0.9}, {"你好", 0.8}, {"你号", 0.7}}, "cjk");
  REQUIRE(r.text == "你好");
  REQUIRE(r.support_votes == 2);
  REQUIRE(std::abs(r.confidence - 0.85) < 1e-9);
}

TEST_CASE("should_accept cjk", "[line_select]") {
  sublift::SubtitleProfile profile;
  profile.script = "cjk";
  REQUIRE(sublift::text::should_accept_text("你好世界", 0.6, profile, 0.5, 0.28, 2));
}

TEST_CASE("bridge_line_boxes finds center anchor accurately", "[line_select]") {
  sublift::SubtitleProfile profile{
      .script = "cjk",
      .center_x = 640,
      .center_y = 46,
      .height = 36,
      .y_min = 10,
      .y_max = 82,
  };
  std::vector<sublift::OcrLine> lines{
      sublift::OcrLine{"左侧文本", 0.90, {150, 28, 120, 36}},
      sublift::OcrLine{"中心字幕", 0.95, {580, 28, 120, 36}},
      sublift::OcrLine{"右侧文本", 0.85, {980, 28, 120, 36}},
  };
  const auto bridged = sublift::text::bridge_line_boxes(lines, profile);
  REQUIRE(bridged.size() == 1);
  CHECK(bridged[0].text == "中心字幕");
  CHECK(bridged[0].box.x == 580);
  CHECK(bridged[0].box.width == 120);
  CHECK(std::abs(bridged[0].confidence - 0.95) < 1e-6);
}

TEST_CASE("bridge_line_boxes merges multi-box long subtitles (22+ characters)", "[line_select]") {
  sublift::SubtitleProfile profile{
      .script = "cjk",
      .center_x = 640,
      .center_y = 46,
      .height = 36,
      .y_min = 10,
      .y_max = 82,
  };
  std::vector<sublift::OcrLine> lines{
      sublift::OcrLine{"这是一句非常长的测试字幕，", 0.90, {380, 28, 220, 36}},
      sublift::OcrLine{"用于验证多框桥接合并", 0.95, {615, 28, 190, 36}},
      sublift::OcrLine{"算法的正确性。", 0.92, {820, 28, 140, 36}},
  };
  const auto bridged = sublift::text::bridge_line_boxes(lines, profile);
  REQUIRE(bridged.size() == 1);
  CHECK(bridged[0].text == "这是一句非常长的测试字幕，用于验证多框桥接合并算法的正确性。");
  CHECK(bridged[0].box.x == 380);
  CHECK(bridged[0].box.width == 960 - 380);
  CHECK(bridged[0].box.y == 28);
  CHECK(bridged[0].box.height == 36);

  // Latin word boundary spacing
  std::vector<sublift::OcrLine> latin_lines{
      sublift::OcrLine{"HELLO", 0.90, {520, 28, 90, 36}},
      sublift::OcrLine{"WORLD", 0.92, {630, 28, 90, 36}},
  };
  const auto latin_bridged = sublift::text::bridge_line_boxes(latin_lines, profile);
  REQUIRE(latin_bridged.size() == 1);
  CHECK(latin_bridged[0].text == "HELLO WORLD");
}

TEST_CASE("bridge_line_boxes and select_line exclude corner watermarks", "[line_select]") {
  sublift::SubtitleProfile profile{
      .script = "cjk",
      .center_x = 640,
      .center_y = 46,
      .height = 36,
      .y_min = 10,
      .y_max = 82,
  };
  std::vector<sublift::OcrLine> lines{
      sublift::OcrLine{"98Tang.COM", 0.98, {22, 30, 110, 24}},
      sublift::OcrLine{"测试字幕第一句", 0.92, {560, 28, 160, 36}},
      sublift::OcrLine{"暖暖的猫", 0.95, {1140, 30, 90, 24}},
  };
  const auto bridged = sublift::text::bridge_line_boxes(lines, profile);
  REQUIRE(bridged.size() == 1);
  CHECK(bridged[0].text == "测试字幕第一句");
  CHECK(bridged[0].box.x == 560);
  CHECK(bridged[0].box.width == 160);

  const auto sel = sublift::text::select_line(lines, profile);
  REQUIRE(sel.has_value());
  CHECK(sel->text == "测试字幕第一句");
}

TEST_CASE("select_line rejects silence frames containing only corner watermarks", "[line_select]") {
  sublift::SubtitleProfile profile{
      .script = "cjk",
      .center_x = 640,
      .center_y = 46,
      .height = 36,
      .y_min = 10,
      .y_max = 82,
  };
  std::vector<sublift::OcrLine> silence_lines{
      sublift::OcrLine{"98Tang.COM", 0.98, {22, 30, 110, 24}},
  };
  const auto bridged = sublift::text::bridge_line_boxes(silence_lines, profile);
  REQUIRE(bridged.empty());

  const auto sel = sublift::text::select_line(silence_lines, profile);
  REQUIRE_FALSE(sel.has_value());
}

TEST_CASE("select_line backward compatibility with uninitialized profile", "[line_select]") {
  sublift::SubtitleProfile uninit_profile{};
  std::vector<sublift::OcrLine> lines{
      sublift::OcrLine{"TEST", 0.90, {10, 10, 100, 20}},
  };
  const auto sel = sublift::text::select_line(lines, uninit_profile);
  REQUIRE(sel.has_value());
  CHECK(sel->text == "TEST");
}

TEST_CASE("adversarial: 6 split boxes very long subtitle (47 characters)", "[line_select][adversarial]") {
  sublift::SubtitleProfile profile{
      .script = "cjk",
      .center_x = 640,
      .center_y = 46,
      .height = 36,
      .y_min = 10,
      .y_max = 82,
  };
  std::vector<sublift::OcrLine> lines{
      sublift::OcrLine{"据了解，", 0.91, {240, 28, 100, 36}},
      sublift::OcrLine{"今年上半年", 0.92, {350, 28, 125, 36}},
      sublift::OcrLine{"全区生产总值实现", 0.95, {485, 28, 200, 36}},
      sublift::OcrLine{"四百二十五点六亿元，", 0.93, {695, 28, 250, 36}},
      sublift::OcrLine{"同比增长", 0.90, {955, 28, 100, 36}},
      sublift::OcrLine{"百分之六点八。", 0.94, {1065, 28, 160, 36}},
  };
  const auto bridged = sublift::text::bridge_line_boxes(lines, profile);
  REQUIRE(bridged.size() == 1);
  const std::string expected = "据了解，今年上半年全区生产总值实现四百二十五点六亿元，同比增长百分之六点八。";
  CHECK(bridged[0].text == expected);
  CHECK(bridged[0].box.x == 240);
  CHECK(bridged[0].box.width == 1225 - 240);
  CHECK(bridged[0].box.y == 28);
  CHECK(bridged[0].box.height == 36);

  std::string concat;
  for (const auto& l : lines) concat += l.text;
  CHECK(bridged[0].text == concat);
}

TEST_CASE("adversarial: box order permutation invariance (720 permutations)", "[line_select][adversarial]") {
  sublift::SubtitleProfile profile{
      .script = "cjk",
      .center_x = 640,
      .center_y = 46,
      .height = 36,
      .y_min = 10,
      .y_max = 82,
  };
  std::vector<sublift::OcrLine> base_lines{
      sublift::OcrLine{"据了解，", 0.91, {240, 28, 100, 36}},
      sublift::OcrLine{"今年上半年", 0.92, {350, 28, 125, 36}},
      sublift::OcrLine{"全区生产总值实现", 0.95, {485, 28, 200, 36}},
      sublift::OcrLine{"四百二十五点六亿元，", 0.93, {695, 28, 250, 36}},
      sublift::OcrLine{"同比增长", 0.90, {955, 28, 100, 36}},
      sublift::OcrLine{"百分之六点八。", 0.94, {1065, 28, 160, 36}},
  };
  const auto canonical = sublift::text::bridge_line_boxes(base_lines, profile);
  REQUIRE(canonical.size() == 1);

  std::vector<int> indices = {0, 1, 2, 3, 4, 5};
  int perm_count = 0;
  bool all_match = true;
  do {
    std::vector<sublift::OcrLine> permuted;
    permuted.reserve(6);
    for (int idx : indices) {
      permuted.push_back(base_lines[idx]);
    }
    const auto res = sublift::text::bridge_line_boxes(permuted, profile);
    if (res.size() != 1 || res[0].text != canonical[0].text ||
        res[0].box.x != canonical[0].box.x ||
        res[0].box.width != canonical[0].box.width ||
        std::abs(res[0].confidence - canonical[0].confidence) > 1e-6) {
      all_match = false;
      break;
    }
    ++perm_count;
  } while (std::next_permutation(indices.begin(), indices.end()));

  CHECK(perm_count == 720);
  CHECK(all_match);
}

TEST_CASE("adversarial: mixed script CJK and Latin spacing", "[line_select][adversarial]") {
  sublift::SubtitleProfile profile{
      .script = "auto",
      .center_x = 640,
      .center_y = 46,
      .height = 36,
      .y_min = 10,
      .y_max = 82,
  };

  SECTION("CJK followed by Latin word with space gap") {
    std::vector<sublift::OcrLine> lines{
        sublift::OcrLine{"欢迎使用", 0.90, {480, 28, 110, 36}},
        sublift::OcrLine{"SubLift", 0.95, {605, 28, 100, 36}},
        sublift::OcrLine{"软件", 0.92, {720, 28, 70, 36}},
    };
    const auto bridged = sublift::text::bridge_line_boxes(lines, profile);
    REQUIRE(bridged.size() == 1);
    CHECK(bridged[0].text == "欢迎使用 SubLift 软件");
  }

  SECTION("Latin sentence with word spaces across 4 split boxes") {
    std::vector<sublift::OcrLine> lines{
        sublift::OcrLine{"THE QUICK", 0.90, {320, 28, 150, 36}},
        sublift::OcrLine{"BROWN FOX", 0.92, {485, 28, 150, 36}},
        sublift::OcrLine{"JUMPS OVER", 0.94, {650, 28, 160, 36}},
        sublift::OcrLine{"THE LAZY DOG", 0.91, {825, 28, 190, 36}},
    };
    const auto bridged = sublift::text::bridge_line_boxes(lines, profile);
    REQUIRE(bridged.size() == 1);
    CHECK(bridged[0].text == "THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG");
  }

  SECTION("Single Latin word split tightly without space gap") {
    std::vector<sublift::OcrLine> lines{
        sublift::OcrLine{"SUPER", 0.90, {560, 28, 75, 36}},
        sublift::OcrLine{"MAN", 0.92, {639, 28, 55, 36}},
    };
    const auto bridged = sublift::text::bridge_line_boxes(lines, profile);
    REQUIRE(bridged.size() == 1);
    CHECK(bridged[0].text == "SUPERMAN");
  }

  SECTION("Pure CJK split boxes with typography gap should NOT insert spaces") {
    std::vector<sublift::OcrLine> lines{
        sublift::OcrLine{"这是第一段", 0.90, {490, 28, 130, 36}},
        sublift::OcrLine{"这是第二段", 0.92, {635, 28, 130, 36}},
    };
    const auto bridged = sublift::text::bridge_line_boxes(lines, profile);
    REQUIRE(bridged.size() == 1);
    CHECK(bridged[0].text == "这是第一段这是第二段");
  }
}

TEST_CASE("adversarial: pathological geometries and boundary conditions", "[line_select][adversarial]") {
  sublift::SubtitleProfile profile{
      .script = "cjk",
      .center_x = 640,
      .center_y = 46,
      .height = 36,
      .y_min = 10,
      .y_max = 82,
  };

  SECTION("Empty lines, whitespace-only lines, and zero/negative dimensions") {
    std::vector<sublift::OcrLine> lines{
        sublift::OcrLine{"", 0.90, {100, 28, 50, 36}},
        sublift::OcrLine{"   \t\n", 0.85, {200, 28, 50, 36}},
        sublift::OcrLine{"有效中心字幕", 0.95, {560, 28, 160, 36}},
        sublift::OcrLine{"零宽度", 0.80, {750, 28, 0, 36}},
        sublift::OcrLine{"零高度", 0.80, {800, 28, 50, 0}},
        sublift::OcrLine{"负坐标", 0.80, {850, 28, -10, -36}},
    };
    const auto bridged = sublift::text::bridge_line_boxes(lines, profile);
    REQUIRE(bridged.size() == 1);
    CHECK(bridged[0].text == "有效中心字幕");
    CHECK(bridged[0].box.x == 560);
    CHECK(bridged[0].box.width == 160);
  }

  SECTION("All empty lines return empty vector") {
    std::vector<sublift::OcrLine> lines{
        sublift::OcrLine{"", 0.90, {560, 28, 160, 36}},
        sublift::OcrLine{"  ", 0.90, {560, 28, 160, 36}},
    };
    const auto bridged = sublift::text::bridge_line_boxes(lines, profile);
    CHECK(bridged.empty());
  }

  SECTION("Empty input returns empty vector") {
    std::vector<sublift::OcrLine> empty_lines;
    const auto bridged = sublift::text::bridge_line_boxes(empty_lines, profile);
    CHECK(bridged.empty());
  }

  SECTION("Slightly overlapping boxes (within 0.25 * h_char)") {
    std::vector<sublift::OcrLine> lines{
        sublift::OcrLine{"前半段", 0.90, {530, 28, 100, 36}},
        sublift::OcrLine{"后半段", 0.92, {624, 28, 100, 36}},
    };
    const auto bridged = sublift::text::bridge_line_boxes(lines, profile);
    REQUIRE(bridged.size() == 1);
    CHECK(bridged[0].text == "前半段后半段");
    CHECK(bridged[0].box.x == 530);
    CHECK(bridged[0].box.width == 724 - 530);
  }

  SECTION("Height discrepancy within tolerance (e.g. 30px vs 36px, ratio = 0.83)") {
    std::vector<sublift::OcrLine> lines{
        sublift::OcrLine{"大字部分", 0.95, {540, 28, 120, 36}},
        sublift::OcrLine{"小字部分", 0.90, {670, 31, 100, 30}},
    };
    const auto bridged = sublift::text::bridge_line_boxes(lines, profile);
    REQUIRE(bridged.size() == 1);
    CHECK(bridged[0].text == "大字部分小字部分");
  }

  SECTION("Severe height discrepancy (e.g. 10px vs 36px, ratio = 0.28 < 0.35) is separated") {
    std::vector<sublift::OcrLine> lines{
        sublift::OcrLine{"中心对白字幕", 0.95, {540, 28, 160, 36}},
        sublift::OcrLine{"tiny", 0.90, {710, 41, 60, 10}},
    };
    const auto bridged = sublift::text::bridge_line_boxes(lines, profile);
    REQUIRE(bridged.size() == 1);
    CHECK(bridged[0].text == "中心对白字幕");
  }

  SECTION("Tall height discrepancy (e.g. 80px banner vs 36px, ratio = 0.45 < 0.70) is separated") {
    std::vector<sublift::OcrLine> lines{
        sublift::OcrLine{"中心对白字幕", 0.95, {540, 28, 160, 36}},
        sublift::OcrLine{"BANNER", 0.90, {710, 6, 60, 80}},
    };
    const auto bridged = sublift::text::bridge_line_boxes(lines, profile);
    REQUIRE(bridged.size() == 1);
    CHECK(bridged[0].text == "中心对白字幕");
  }

  SECTION("Vertical band filtering excludes lines outside profile y_band") {
    std::vector<sublift::OcrLine> lines{
        sublift::OcrLine{"远方顶部文本", 0.90, {580, -50, 120, 30}},
        sublift::OcrLine{"正常中心字幕", 0.95, {580, 28, 120, 36}},
        sublift::OcrLine{"底部界外文本", 0.90, {580, 120, 120, 36}},
    };
    const auto bridged = sublift::text::bridge_line_boxes(lines, profile);
    REQUIRE(bridged.size() == 1);
    CHECK(bridged[0].text == "正常中心字幕");
  }
}

TEST_CASE("adversarial bug repro: punctuation, short words, tie-breaking and overlap", "[line_select][adversarial][bug_repro]") {
  sublift::SubtitleProfile profile{
      .script = "cjk",
      .center_x = 640,
      .center_y = 46,
      .height = 36,
      .y_min = 10,
      .y_max = 82,
  };

  SECTION("BUG 1: dash at center axis with height 16 blocks flanking text") {
    std::vector<sublift::OcrLine> lines{
        sublift::OcrLine{"第1集", 0.95, {500, 28, 80, 36}},
        sublift::OcrLine{"——", 0.90, {590, 38, 60, 16}},
        sublift::OcrLine{"启程", 0.95, {660, 28, 70, 36}},
    };
    const auto bridged = sublift::text::bridge_line_boxes(lines, profile);
    REQUIRE(bridged.size() == 1);
    CHECK(bridged[0].text == "第1集——启程");
  }

  SECTION("BUG 2: quotation marks with height 16 flanking 36px text are dropped") {
    std::vector<sublift::OcrLine> lines{
        sublift::OcrLine{"“", 0.90, {530, 30, 20, 16}},
        sublift::OcrLine{"这是对白", 0.95, {555, 28, 120, 36}},
        sublift::OcrLine{"”", 0.90, {680, 30, 20, 16}},
    };
    const auto bridged = sublift::text::bridge_line_boxes(lines, profile);
    REQUIRE(bridged.size() == 1);
    CHECK(bridged[0].text == "“这是对白”");
  }

  SECTION("BUG 3: tied center distance picks first in vector regardless of vertical match") {
    std::vector<sublift::OcrLine> lines_logo_first{
        sublift::OcrLine{"顶部台标", 0.90, {580, 2, 120, 16}},
        sublift::OcrLine{"中心字幕", 0.95, {580, 28, 120, 36}},
    };
    std::vector<sublift::OcrLine> lines_subtitle_first{
        sublift::OcrLine{"中心字幕", 0.95, {580, 28, 120, 36}},
        sublift::OcrLine{"顶部台标", 0.90, {580, 2, 120, 16}},
    };
    const auto res1 = sublift::text::bridge_line_boxes(lines_logo_first, profile);
    const auto res2 = sublift::text::bridge_line_boxes(lines_subtitle_first, profile);
    REQUIRE(res1.size() == 1);
    REQUIRE(res2.size() == 1);
    CHECK(res1[0].text == res2[0].text);
  }

  SECTION("BUG 4: short Latin word 'a' (h=20) between CJK blocks (h=36) is silently erased") {
    std::vector<sublift::OcrLine> lines{
        sublift::OcrLine{"这是一个", 0.95, {480, 28, 100, 36}},
        sublift::OcrLine{"a", 0.90, {590, 36, 18, 20}},
        sublift::OcrLine{"重要测试", 0.95, {618, 28, 100, 36}},
    };
    const auto bridged = sublift::text::bridge_line_boxes(lines, profile);
    REQUIRE(bridged.size() == 1);
    CHECK(bridged[0].text == "这是一个 a 重要测试");
  }

  SECTION("BUG 5: overlapping boxes exceeding 0.25 * h_char (20px overlap) drops box") {
    std::vector<sublift::OcrLine> lines{
        sublift::OcrLine{"前半部分", 0.95, {450, 28, 120, 36}},
        sublift::OcrLine{"后半部分", 0.95, {550, 28, 120, 36}},
    };
    const auto bridged = sublift::text::bridge_line_boxes(lines, profile);
    REQUIRE(bridged.size() == 1);
    CHECK(bridged[0].text == "前半部分后半部分");
  }
}

TEST_CASE("is_blank_text recognizes Unicode whitespace", "[text][srt]") {
  // Empty & ASCII whitespace
  CHECK(sublift::text::is_blank_text(""));
  CHECK(sublift::text::is_blank_text("   \t\n\r\f\v"));

  // Unicode whitespace: ideographic space U+3000 (全角空格), NBSP U+00A0
  CHECK(sublift::text::is_blank_text("\u3000"));
  CHECK(sublift::text::is_blank_text("\u00A0"));
  CHECK(sublift::text::is_blank_text("  \u3000  \u00A0 \t "));

  // Non-blank text
  CHECK_FALSE(sublift::text::is_blank_text("a"));
  CHECK_FALSE(sublift::text::is_blank_text("你好"));
  CHECK_FALSE(sublift::text::is_blank_text("\u3000你好\u3000"));
}

TEST_CASE("should_accept_text rejects pure Unicode whitespace", "[text][consensus]") {
  sublift::SubtitleProfile auto_profile{.script = "auto"};
  sublift::SubtitleProfile cjk_profile{.script = "cjk"};

  // Must reject pure Unicode whitespace regardless of high confidence or support votes
  CHECK_FALSE(sublift::text::should_accept_text("\u3000", 0.99, auto_profile, 0.5, 0.28, 10));
  CHECK_FALSE(sublift::text::should_accept_text("\u00A0", 0.99, auto_profile, 0.5, 0.28, 10));
  CHECK_FALSE(sublift::text::should_accept_text(" \u3000 \u00A0 ", 0.99, auto_profile, 0.5, 0.28, 10));
  CHECK_FALSE(sublift::text::should_accept_text("\u3000", 0.99, cjk_profile, 0.5, 0.28, 10));

  // Must accept valid text with embedded or edge Unicode whitespace
  CHECK(sublift::text::should_accept_text("\u3000你好世界\u3000", 0.9, auto_profile, 0.5, 0.28, 2));
}

TEST_CASE("format_srt skips pure Unicode whitespace entries", "[text][srt]") {
  std::vector<sublift::SubtitleEntry> entries = {
      {.id = 1, .range = {1000, 2000}, .text = "\u3000", .confidence = 1.0},
      {.id = 2, .range = {2000, 3000}, .text = "有效字幕", .confidence = 0.9},
      {.id = 3, .range = {3000, 4000}, .text = "  \u00A0  ", .confidence = 1.0},
  };
  auto [srt, count] = sublift::text::format_srt_counted(entries);
  CHECK(count == 1);
  CHECK(srt.find("1\n00:00:02,000 --> 00:00:03,000\n有效字幕") != std::string::npos);
  CHECK(srt.find("\u3000") == std::string::npos);
}

TEST_CASE("select_line honors unified wide_aspect_threshold parameter", "[text][line_select]") {
  auto profile = sublift::SubtitleProfile::from_crop(1920, 320, "cjk");
  // Crop is 1920x320 => aspect = 6.0
  std::vector<sublift::OcrLine> split_clauses{
      sublift::OcrLine{"可以收工啦，谢啰，", 0.96, {620, 180, 320, 44}},
      sublift::OcrLine{"长舌妇", 0.95, {952, 180, 120, 44}},
  };

  // With threshold 5.0 (aspect 6.0 > 5.0 => wide strip), clauses on the same row are bridged
  auto bridged = sublift::text::select_line(split_clauses, profile, 0.25, 0.5, 5.0);
  REQUIRE(bridged.has_value());
  CHECK(bridged->text == "可以收工啦，谢啰，长舌妇");

  // With threshold 8.0 (aspect 6.0 <= 8.0 => tall ROI), single best clause is picked without wide bridging
  auto unbridged = sublift::text::select_line(split_clauses, profile, 0.25, 0.5, 8.0);
  REQUIRE(unbridged.has_value());
  CHECK(unbridged->text != "可以收工啦，谢啰，长舌妇");
}

TEST_CASE("line_select & consensus resilience against NaN confidences", "[text][line_select][regression]") {
  const double qnan = std::numeric_limits<double>::quiet_NaN();
  auto profile = sublift::SubtitleProfile::from_crop(1920, 1080, "cjk");

  SECTION("score_line does not produce NaN for NaN line confidence") {
    OcrLine line_nan{
        .text = "测试字幕NaN",
        .confidence = qnan,
        .box = OcrCropBox{600, 900, 720, 40},
    };
    LineScore score = score_line(line_nan, profile);
    CHECK_FALSE(std::isnan(score.confidence));
    CHECK_FALSE(std::isnan(score.total));
    CHECK(score.confidence == 0.0);
  }

  SECTION("bridge_line_boxes does not produce NaN confidence when chain has NaN") {
    std::vector<OcrLine> chain_lines = {
        OcrLine{.text = "前半句", .confidence = 0.90, .box = OcrCropBox{600, 900, 200, 40}},
        OcrLine{.text = "后半句NaN", .confidence = qnan, .box = OcrCropBox{810, 900, 200, 40}},
    };
    auto bridged = bridge_line_boxes(chain_lines, profile);
    REQUIRE_FALSE(bridged.empty());
    CHECK_FALSE(std::isnan(bridged[0].confidence));
  }

  SECTION("consensus_retry_needed triggers retry for NaN confidence") {
    CHECK(consensus_retry_needed("测试字幕", qnan, 0.85));
  }

  SECTION("consensus_text does not produce NaN confidence for NaN samples") {
    std::vector<std::pair<std::string, double>> samples = {
        {"测试字幕", qnan},
        {"测试字幕", 0.90},
    };
    auto res = consensus_text(samples, "cjk");
    CHECK_FALSE(std::isnan(res.confidence));
    CHECK(res.confidence > 0.0);
  }
}

TEST_CASE("Pure consensus decision functions", "[text][consensus]") {
  sublift::SubtitleProfile profile = sublift::SubtitleProfile::from_crop(100, 40, "cjk");
  sublift::text::ConsensusResult partial{
      .text = "测试文本",
      .confidence = 0.90,
      .support_votes = 2,
  };

  REQUIRE(sublift::text::should_early_terminate_consensus(
      partial, "测试文本", 0.95, profile, 0.50, 0.28, 0.12));

  sublift::text::ConsensusResult partial_low{
      .text = "test",
      .confidence = 0.20,
      .support_votes = 1,
  };
  REQUIRE_FALSE(sublift::text::should_early_terminate_consensus(
      partial_low, "test", 0.20, profile, 0.50, 0.28, 0.12));

  REQUIRE(sublift::text::consensus_retry_needed("", 0.9, 0.5));
  REQUIRE(sublift::text::consensus_retry_needed("abc", 0.3, 0.5));
  REQUIRE_FALSE(sublift::text::consensus_retry_needed("abc", 0.8, 0.5));
}



