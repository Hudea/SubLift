#include <catch2/catch_test_macros.hpp>

#include "sublift/dedupe.hpp"

namespace {
sublift::SubtitleEntry e(std::int64_t s, std::int64_t en, const char* t,
                         double c = 1.0) {
  return sublift::SubtitleEntry{s, en, t, c};
}
}  // namespace

TEST_CASE("merge same adjacent", "[dedupe]") {
  const auto out = sublift::merge_entries(
      {e(0, 1000, "你好", 1.0), e(1100, 2000, "你好", 0.9)}, 1000, 0, false);
  REQUIRE(out.size() == 1);
  REQUIRE(out[0].start_ms == 0);
  REQUIRE(out[0].end_ms == 2000);
  REQUIRE(out[0].text == "你好");
  REQUIRE(out[0].confidence == 1.0);
}

TEST_CASE("no merge large gap", "[dedupe]") {
  const auto out = sublift::merge_entries(
      {e(0, 1000, "你好"), e(3000, 4000, "你好")}, 1000, 0, false);
  REQUIRE(out.size() == 2);
}

TEST_CASE("filter short", "[dedupe]") {
  const auto out = sublift::merge_entries(
      {e(0, 1000, "长"), e(1100, 1200, "短")}, 1000, 500, false);
  REQUIRE(out.size() == 1);
  REQUIRE(out[0].text == "长");
}

TEST_CASE("drop empty text", "[dedupe]") {
  const auto kept = sublift::merge_entries(
      {e(0, 1000, "有字"), e(1100, 2000, "  ")}, 1000, 0, false);
  REQUIRE(kept.size() == 2);
  const auto dropped = sublift::merge_entries(
      {e(0, 1000, "有字"), e(1100, 2000, "  ")}, 1000, 0, true);
  REQUIRE(dropped.size() == 1);
}

TEST_CASE("whitespace normalize merge", "[dedupe]") {
  const auto out = sublift::merge_entries(
      {e(0, 1000, "你好"), e(1000, 2000, " 你 好 ")}, 1000, 0, false);
  REQUIRE(out.size() == 1);
  REQUIRE(out[0].text == "你好");
}

TEST_CASE("empty between same text merges without drop_empty_text", "[dedupe]") {
  const auto out = sublift::merge_entries(
      {e(0, 4200, "放开我"), e(4200, 5000, ""), e(5000, 5800, "放开我")},
      1000, 500, false);
  REQUIRE(out.size() == 1);
  REQUIRE(out[0].start_ms == 0);
  REQUIRE(out[0].end_ms == 5800);
  REQUIRE(out[0].text == "放开我");
}

TEST_CASE("typewriter watermark fragments merge to longest", "[dedupe]") {
  const auto out = sublift::merge_entries(
      {e(0, 600, "更"), e(600, 1400, "多"), e(1400, 3200, "多资源尽在98堂"),
       e(3400, 5000, "更多资源尽在98堂"),
       e(5000, 6600, "更多资源尽在98堂永久地址："),
       e(6600, 7400, "更多资源尽在98堂永久地址：WWW.98T.LA"),
       e(8000, 8800, "更多资源尽在98堂永久地址："),
       e(8800, 9400, "更多资源尽在98堂")},
      1000, 0, false);
  REQUIRE(out.size() == 1);
  REQUIRE(out[0].start_ms == 0);
  REQUIRE(out[0].end_ms == 9400);
  REQUIRE(out[0].text.find("WWW.98T.LA") != std::string::npos);
}

TEST_CASE("single char not absorbed unless prefix", "[dedupe]") {
  const auto out = sublift::merge_entries(
      {e(0, 800, "嘛"), e(800, 2800, "知道啦进公司后就禁止")}, 1000, 0, false);
  REQUIRE(out.size() == 2);
}

TEST_CASE("OCR insertion variant merges to shorter text", "[dedupe]") {
  const auto out = sublift::merge_entries(
      {e(0, 1000, "哈茱蒂，本市第一位兔警员", 0.7),
       e(1000, 1800, "哈茱蒂，Y本市第一位兔警员", 0.9)},
      1000, 0, false);
  REQUIRE(out.size() == 1);
  REQUIRE(out[0].text == "哈茱蒂，本市第一位兔警员");
  REQUIRE(out[0].end_ms == 1800);
  REQUIRE(out[0].confidence == 0.9);
}

TEST_CASE("OCR substitution variant is not merged", "[dedupe]") {
  const auto out = sublift::merge_entries(
      {e(0, 1000, "第一组免狐搭档", 0.7),
       e(1000, 2000, "第一组兔狐搭档", 0.9)},
      1000, 0, false);
  REQUIRE(out.size() == 2);
}

TEST_CASE("containment keeps longer text over confidence", "[dedupe]") {
  const auto out = sublift::merge_entries(
      {e(0, 1000, "跟胡尼克", 0.99),
       e(1000, 2000, "跟胡尼克，一位平凡的狐狸", 0.80)},
      1000, 0, false);
  REQUIRE(out.size() == 1);
  REQUIRE(out[0].text == "跟胡尼克，一位平凡的狐狸");
  REQUIRE(out[0].confidence == 0.99);
}

TEST_CASE("similar variant does not merge across a large gap", "[dedupe]") {
  const auto out = sublift::merge_entries(
      {e(0, 1000, "哈茱蒂，本市第一位兔警员", 0.9),
       e(1500, 2500, "哈茱蒂，Y本市第一位兔警员", 0.7)},
      1000, 0, false);
  REQUIRE(out.size() == 2);
}

// ===========================================================================
// Phase 21 D03: entry-level persistent-watermark suppression
// ===========================================================================

namespace {

// 32s synthetic film timeline: one scrolling watermark family re-read in the
// dialogue gaps (variants incl. an OCR one-char slip), plus two real cues.
// 7 entries >= 5; union coverage 7s >= 8% * 32s; span 30s >= 60% * 32s.
std::vector<sublift::SubtitleEntry> scrolling_watermark_family() {
  return {
      e(0, 1000, "暖暖的猫"),
      e(4000, 5000, "暖暖的猫98"),
      e(9000, 10000, "暖暖的猫"),
      e(14000, 15000, "暖暖的猫98Tang.COM"),
      e(19000, 20000, "暖暖的猫"),
      e(24000, 25000, "暖的猫"),
      e(29000, 30000, "暖暖的猫98"),
  };
}

}  // namespace

TEST_CASE("suppress_watermark_entries: empty and short inputs are safe",
          "[watermark]") {
  REQUIRE(sublift::suppress_watermark_entries({}, 32000).empty());
  // A single watermark cue is not a full-film family -> unchanged.
  const std::vector<sublift::SubtitleEntry> one{e(0, 1000, "暖暖的猫98Tang.COM")};
  const auto kept = sublift::suppress_watermark_entries(one, 32000);
  REQUIRE(kept.size() == 1);
  REQUIRE(kept[0].text == one[0].text);
  // Unknown video duration -> no-op (never remove on missing evidence).
  const auto kept_no_duration =
      sublift::suppress_watermark_entries(one, /*video_duration_ms=*/0);
  REQUIRE(kept_no_duration.size() == 1);
}

TEST_CASE("suppress_watermark_entries: persistent family is removed",
          "[watermark]") {
  const auto entries = scrolling_watermark_family();
  const auto out = sublift::suppress_watermark_entries(entries, 32000);
  REQUIRE(out.empty());
}

TEST_CASE("suppress_watermark_entries: dialogue entries are preserved",
          "[watermark]") {
  std::vector<sublift::SubtitleEntry> entries{
      e(0, 1000, "暖暖的猫"),
      e(1000, 2000, "测试字幕第一句", 0.99),
      e(4000, 5000, "暖暖的猫98"),
      e(5000, 6000, "第三句台词内容", 0.98),
      e(9000, 10000, "暖暖的猫"),
      e(14000, 15000, "暖暖的猫98Tang.COM"),
      e(19000, 20000, "暖暖的猫"),
      e(24000, 25000, "暖的猫"),
      e(29000, 30000, "暖暖的猫98"),
  };
  const auto out = sublift::suppress_watermark_entries(entries, 32000);
  REQUIRE(out.size() == 2);
  REQUIRE(out[0].text == "测试字幕第一句");
  REQUIRE(out[1].text == "第三句台词内容");
  REQUIRE(out[0].start_ms == 1000);
  REQUIRE(out[1].end_ms == 6000);
}

TEST_CASE("suppress_watermark_entries: repeated short line below coverage kept",
          "[watermark]") {
  // "谢谢" 10 times over a 10-minute video: coverage 10s = 1.7% < 8% -> keep.
  std::vector<sublift::SubtitleEntry> entries;
  for (int i = 0; i < 10; ++i) {
    entries.push_back(e(static_cast<std::int64_t>(i) * 60'000,
                        static_cast<std::int64_t>(i) * 60'000 + 1000, "谢谢"));
  }
  const auto out = sublift::suppress_watermark_entries(entries, 600'000);
  REQUIRE(out.size() == 10);
}

TEST_CASE("suppress_watermark_entries: family below min count kept",
          "[watermark]") {
  // 4 URLish entries: coverage 16s / 32s and span 28s / 32s are high enough,
  // but the family needs >= 5 entries.
  const std::vector<sublift::SubtitleEntry> entries{
      e(0, 4000, "暖暖的猫98Tang.COM"),
      e(8000, 12000, "暖暖的猫98Tang.COM"),
      e(16000, 20000, "暖暖的猫98Tang.COM"),
      e(24000, 28000, "暖暖的猫98Tang.COM"),
  };
  const auto out = sublift::suppress_watermark_entries(entries, 32000);
  REQUIRE(out.size() == 4);
}

TEST_CASE("suppress_watermark_entries: family below coverage kept",
          "[watermark]") {
  // 6 identical cues over a 10-minute video: 6s total = 1% < 8% -> keep
  // (even though the 500s span is > 60% of the video).
  std::vector<sublift::SubtitleEntry> entries;
  for (int i = 0; i < 6; ++i) {
    entries.push_back(e(static_cast<std::int64_t>(i) * 100'000,
                        static_cast<std::int64_t>(i) * 100'000 + 1000,
                        "暖暖的猫98Tang.COM"));
  }
  const auto out = sublift::suppress_watermark_entries(entries, 600'000);
  REQUIRE(out.size() == 6);
}

TEST_CASE("suppress_watermark_entries: identical URL overlay family removed",
          "[watermark]") {
  // 7 identical URL overlay cues over 128s: single variant, but the URL is a
  // strong watermark signal, so the family is removed.
  std::vector<sublift::SubtitleEntry> entries;
  for (int i = 0; i < 7; ++i) {
    entries.push_back(e(static_cast<std::int64_t>(i) * 18'000,
                        static_cast<std::int64_t>(i) * 18'000 + 2'400,
                        "暖暖的猫98Tang.COM"));
  }
  const auto out = sublift::suppress_watermark_entries(entries, 127'800);
  REQUIRE(out.empty());
}

TEST_CASE("suppress_watermark_entries: repeated identical dialogue kept",
          "[watermark]") {
  // Regression for the Phase 21 128s loop A/B: the same normal cue repeats
  // every 18s, so count (7 >= 5), coverage (16.8s = 13%) and span (77%) all
  // pass the numeric thresholds. It is exact-identical dialogue with no URL
  // evidence and exactly one variant -> the safety guard must keep it.
  std::vector<sublift::SubtitleEntry> entries;
  for (int i = 0; i < 7; ++i) {
    entries.push_back(e(static_cast<std::int64_t>(i) * 18'000,
                        static_cast<std::int64_t>(i) * 18'000 + 2'400,
                        "测试字幕第一句"));
  }
  const auto out = sublift::suppress_watermark_entries(entries, 127'800);
  REQUIRE(out.size() == 7);
  for (const auto& entry : out) {
    REQUIRE(entry.text == "测试字幕第一句");
  }
}

TEST_CASE("suppress_watermark_entries: blank entries are never candidates",
          "[watermark]") {
  // Blank cues are merge inputs for adjacent dialogue; the filter must not
  // touch them even when a watermark family is removed.
  std::vector<sublift::SubtitleEntry> entries{
      e(0, 1000, "暖暖的猫"),
      e(1000, 3000, ""),
      e(4000, 5000, "暖暖的猫98"),
      e(5000, 6000, "  "),
      e(9000, 10000, "暖暖的猫"),
      e(14000, 15000, "暖暖的猫98Tang.COM"),
      e(19000, 20000, "暖暖的猫"),
      e(24000, 25000, "暖的猫"),
      e(29000, 30000, "暖暖的猫98"),
  };
  const auto out = sublift::suppress_watermark_entries(entries, 32000);
  REQUIRE(out.size() == 2);
  REQUIRE(out[0].text.empty());
  REQUIRE(out[1].text == "  ");
}
