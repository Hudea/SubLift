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

// ===========================================================================
// Phase 21 D03: watermark text suppression (line level)
// ===========================================================================

namespace {
sublift::SubtitleProfile cjk_band_profile() {
  sublift::SubtitleProfile profile;
  profile.script = "cjk";
  profile.center_x = 160;
  profile.center_y = 40;
  profile.height = 30;
  profile.y_min = 20;
  profile.y_max = 60;
  return profile;
}
}  // namespace

TEST_CASE("watermark predicate matches URL/domain overlay text", "[watermark]") {
  // Scrolling watermark seen in debug/watermark-repro fixtures.
  REQUIRE(sublift::is_watermark_text("暖暖的猫 98Tang.COM"));
  REQUIRE(sublift::is_watermark_text("暖暖的猫98Tang.COM"));
  REQUIRE(sublift::is_watermark_text("暖暖的猫 98Tang.CC"));
  // Typewriter watermark with a "永久地址" URL.
  REQUIRE(sublift::is_watermark_text("更多资源尽在98堂永久地址：WWW.98T.LA"));
  // Explicit scheme / www forms, case-insensitive.
  REQUIRE(sublift::is_watermark_text("http://example.com/x"));
  REQUIRE(sublift::is_watermark_text("HTTPS://EXAMPLE.COM"));
  REQUIRE(sublift::is_watermark_text("www.foo.cn"));
  REQUIRE(sublift::is_watermark_text("电影天堂 www.dytt8.net"));
  REQUIRE(sublift::is_watermark_text("98Tang.COM"));
  REQUIRE(sublift::is_watermark_text("foo.tv"));
}

TEST_CASE("watermark predicate keeps normal dialogue", "[watermark]") {
  REQUIRE_FALSE(sublift::is_watermark_text("你好世界"));
  REQUIRE_FALSE(sublift::is_watermark_text("任务代号 FOX TWO"));
  REQUIRE_FALSE(sublift::is_watermark_text("欢迎来到 ZPD"));
  REQUIRE_FALSE(sublift::is_watermark_text("（前情提要…）"));
  REQUIRE_FALSE(sublift::is_watermark_text("HELLO"));
  REQUIRE_FALSE(sublift::is_watermark_text("3.14 是圆周率"));
  REQUIRE_FALSE(sublift::is_watermark_text("Dr. Smith"));
  REQUIRE_FALSE(sublift::is_watermark_text("U.S. 政府"));
  REQUIRE_FALSE(sublift::is_watermark_text(""));
}

TEST_CASE("score_line caps watermark line below min select score", "[watermark]") {
  const auto profile = cjk_band_profile();
  // Same geometry as a well-placed subtitle line: components are high, but the
  // watermark cap must sink total below the default min_score (0.28).
  const sublift::OcrLine watermark{"暖暖的猫 98Tang.COM", 0.99, {100, 30, 120, 28}};
  const auto wm_score = sublift::score_line(watermark, profile);
  REQUIRE(wm_score.total < 0.28);
  REQUIRE(wm_score.total < 0.10);
  // The identical geometry with normal dialogue is unaffected.
  const sublift::OcrLine dialogue{"你好世界", 0.99, {100, 30, 120, 28}};
  const auto dl_score = sublift::score_line(dialogue, profile);
  REQUIRE(dl_score.total >= 0.28);
  REQUIRE(dl_score.total > wm_score.total);
}

TEST_CASE("watermark predicate matches clipped URL mixed token", "[watermark]") {
  // Scrolling overlay clipped before the domain dot (real 5fps OCR output).
  REQUIRE(sublift::is_watermark_text("暖暖的猫 98Ta"));
  REQUIRE(sublift::is_watermark_text("暖暖的猫 98Tang"));
  REQUIRE(sublift::is_watermark_text("暖暖的猫98Tang"));
  // Digit runs without a letter tail, product names and unit tokens stay safe.
  REQUIRE_FALSE(sublift::is_watermark_text("第2集"));
  REQUIRE_FALSE(sublift::is_watermark_text("1080P 画质"));
  REQUIRE_FALSE(sublift::is_watermark_text("iPhone14 真不错"));
  REQUIRE_FALSE(sublift::is_watermark_text("X战警2"));
  REQUIRE_FALSE(sublift::is_watermark_text("20th century"));
  REQUIRE_FALSE(sublift::is_watermark_text("任务代号 FOX TWO"));
}

TEST_CASE("watermark predicate matches partial/unknown-TLD domain fragments",
          "[watermark]") {
  // 128s loop A/B: scrolling overlay clipped after the dot / unknown TLD.
  REQUIRE(sublift::is_watermark_text("暖暖的猫 98Tang.cor第四句测试文本"));
  REQUIRE(sublift::is_watermark_text("暖暖的猫 98Tang.cor"));
  REQUIRE(sublift::is_watermark_text("暖暖的猫 98Tang."));
  // "1080p." has only one letter after the digits: not domain evidence.
  REQUIRE_FALSE(sublift::is_watermark_text("1080p. 画质"));
  REQUIRE_FALSE(sublift::is_watermark_text("3.14 是圆周率"));
}

TEST_CASE("select_line salvages CJK dialogue glued to a URL overlay", "[watermark]") {
  const auto profile = cjk_band_profile();
  // Real OCR pollution from the synthetic scrolling-watermark clip: the
  // overlay is glued to the subtitle; cleanup must keep the Chinese dialogue.
  const std::vector<sublift::OcrLine> glued{
      sublift::OcrLine{"第三句台词内容Tang.COM", 0.94, {100, 30, 120, 28}},
  };
  const auto salvaged = sublift::select_line(glued, profile);
  REQUIRE(salvaged.has_value());
  REQUIRE(salvaged->text == "第三句台词内容");
  // 128s loop A/B: overlay lands in the middle of the subtitle line; the CJK
  // remainder after the URL run is the real dialogue.
  const std::vector<sublift::OcrLine> glued_middle{
      sublift::OcrLine{"暖暖的猫 98Tang.cor第四句测试文本", 0.96,
                       {100, 30, 120, 28}},
  };
  const auto middle = sublift::select_line(glued_middle, profile);
  REQUIRE(middle.has_value());
  REQUIRE(middle->text == "第四句测试文本");
  // URL-only overlay still cannot be selected even after cleanup (the nickname
  // prefix is too short to be a plausible subtitle).
  const std::vector<sublift::OcrLine> url_only{
      sublift::OcrLine{"暖暖的猫 98Tang.COM", 0.99, {100, 30, 120, 28}},
  };
  REQUIRE_FALSE(sublift::select_line(url_only, profile).has_value());
}

TEST_CASE("select_line treats split overlay CJK fragment as watermark",
          "[watermark]") {
  const auto profile = cjk_band_profile();
  // Real boxes from the 5fps run: the scrolling overlay is split into
  // "暖暖的猫" + "苗98Tang.COM"; the fragment alone would otherwise win.
  const sublift::OcrLine fragment{"暖暖的猫", 1.0, {680, 118, 108, 33}};
  const sublift::OcrLine overlay_url{"苗98Tang.COM", 0.97, {790, 118, 180, 35}};
  const std::vector<sublift::OcrLine> frame_no_dialogue{fragment, overlay_url};
  REQUIRE_FALSE(sublift::select_line(frame_no_dialogue, profile).has_value());

  // With a real (taller, centered) subtitle line in the same frame, dialogue
  // wins and the fragment stays suppressed.
  const std::vector<sublift::OcrLine> frame_with_dialogue{
      fragment,
      overlay_url,
      sublift::OcrLine{"正常字幕内容", 0.90, {400, 60, 480, 56}},
  };
  const auto selected = sublift::select_line(frame_with_dialogue, profile);
  REQUIRE(selected.has_value());
  REQUIRE(selected->text == "正常字幕内容");
}

TEST_CASE("select_line keeps short dialogue next to an overlay", "[watermark]") {
  const auto profile = cjk_band_profile();
  // Subtitle glyphs are taller than the overlay (56 vs 35 px): the fragment
  // rule must not swallow a short real line such as "走吧".
  const std::vector<sublift::OcrLine> frame{
      sublift::OcrLine{"走吧", 0.99, {600, 40, 80, 56}},
      sublift::OcrLine{"苗98Tang.COM", 0.97, {790, 118, 180, 35}},
  };
  const auto selected = sublift::select_line(frame, profile);
  REQUIRE(selected.has_value());
  REQUIRE(selected->text == "走吧");
}

TEST_CASE("select_line cannot distinguish a bare CJK fragment without context",
          "[watermark]") {
  // Documented limit: with no URL line / cleanup evidence in frame, a pure-CJK
  // nickname fragment is indistinguishable from dialogue at line level and is
  // left to the entry-level repetition filter.
  const auto profile = cjk_band_profile();
  const std::vector<sublift::OcrLine> frame{
      sublift::OcrLine{"暖暖的猫", 1.0, {100, 30, 120, 28}},
  };
  const auto selected = sublift::select_line(frame, profile);
  REQUIRE(selected.has_value());
  REQUIRE(selected->text == "暖暖的猫");
}

TEST_CASE("select_line never selects a watermark line", "[watermark]") {
  const auto profile = cjk_band_profile();
  // Watermark-only frame -> no line is selectable.
  const std::vector<sublift::OcrLine> watermark_only{
      sublift::OcrLine{"暖暖的猫 98Tang.COM", 0.99, {100, 30, 120, 28}},
      sublift::OcrLine{"98Tang.COM", 0.99, {100, 30, 120, 28}},
  };
  REQUIRE_FALSE(sublift::select_line(watermark_only, profile).has_value());

  // Dialogue + watermark: dialogue wins even with lower confidence. Subtitle
  // glyphs are taller than the overlay line (45 vs 28 px), which is what keeps
  // the split-fragment rule from swallowing real dialogue.
  const std::vector<sublift::OcrLine> mixed{
      sublift::OcrLine{"暖暖的猫 98Tang.COM", 0.99, {100, 30, 120, 28}},
      sublift::OcrLine{"正常字幕内容", 0.80, {20, 5, 200, 45}},
  };
  const auto selected = sublift::select_line(mixed, profile);
  REQUIRE(selected.has_value());
  REQUIRE(selected->text == "正常字幕内容");
}
