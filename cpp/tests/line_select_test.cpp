#include <catch2/catch_test_macros.hpp>

#include "sublift/line_select.hpp"

#include <cmath>

TEST_CASE("normalize_ocr_text collapses whitespace", "[line_select]") {
  REQUIRE(sublift::normalize_ocr_text("  你  好  ") == "你 好");
}

TEST_CASE("cleanup ellipsis and quotes", "[line_select]") {
  REQUIRE(sublift::cleanup_subtitle_text("你好...", "cjk") == "你好…");
  REQUIRE(sublift::cleanup_subtitle_text("（前情提要⋯）", "cjk") ==
          "（前情提要…）");
  REQUIRE(sublift::cleanup_subtitle_text("他们说：『没问题』", "cjk") ==
          "他们说：「没问题」");
  REQUIRE(sublift::cleanup_subtitle_text("事实上.", "cjk") == "事实上…");
}

TEST_CASE("cleanup strips only latin attached to CJK edges", "[line_select]") {
  // Python tests/test_line_select.py::TestCleanup
  REQUIRE(sublift::cleanup_subtitle_text("（前市长杨咩咩入狱）PHISON", "cjk") ==
          "（前市长杨咩咩入狱）");
  REQUIRE(sublift::cleanup_subtitle_text("在动物方城市气候墙SON", "cjk") ==
          "在动物方城市气候墙");
  REQUIRE(sublift::cleanup_subtitle_text("FOR一起揭穿阴谋SON", "cjk") ==
          "一起揭穿阴谋");
  REQUIRE(sublift::cleanup_subtitle_text(
              "哈茱蒂，本市第一位兔警员EFS CONSPI _N/、", "cjk") ==
          "哈茱蒂，本市第一位兔警员");
}

TEST_CASE("cleanup keeps legitimate mixed English", "[line_select]") {
  REQUIRE(sublift::cleanup_subtitle_text("HELLO", "cjk") == "HELLO");
  REQUIRE(sublift::cleanup_subtitle_text("欢迎来到 ZPD", "cjk") ==
          "欢迎来到 ZPD");
  REQUIRE(sublift::cleanup_subtitle_text("任务代号 FOX TWO", "cjk") ==
          "任务代号 FOX TWO");
  REQUIRE(sublift::cleanup_subtitle_text("后来加入ZPD警局", "cjk") ==
          "后来加入ZPD警局");
}

TEST_CASE("normalize unicode whitespace", "[line_select]") {
  // Ideographic space U+3000 and NBSP should collapse like Python \s
  const std::string ideo = "你\u3000好";
  const std::string nbsp = "你\u00a0好";
  REQUIRE(sublift::normalize_ocr_text(ideo) == "你 好");
  REQUIRE(sublift::normalize_ocr_text(nbsp) == "你 好");
}

TEST_CASE("edit_distance kitten/sitting", "[line_select]") {
  REQUIRE(sublift::edit_distance("kitten", "sitting") == 3);
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
  const auto sel = sublift::select_line(lines, profile);
  REQUIRE(sel.has_value());
  REQUIRE(sel->text == "你好世界");
}

TEST_CASE("consensus_text cjk majority", "[line_select]") {
  const auto r = sublift::consensus_text(
      {{"你好", 0.9}, {"你好", 0.8}, {"你号", 0.7}}, "cjk");
  REQUIRE(r.text == "你好");
  REQUIRE(r.support_votes == 2);
  REQUIRE(std::abs(r.confidence - 0.85) < 1e-9);
}

TEST_CASE("should_accept cjk", "[line_select]") {
  sublift::SubtitleProfile profile;
  profile.script = "cjk";
  REQUIRE(sublift::should_accept_text("你好世界", 0.6, profile, 0.5, 0.28, 2));
}
