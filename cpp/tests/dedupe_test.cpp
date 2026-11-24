#include <cstdint>
#include <limits>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <sublift/core/subtitle.hpp>
#include <sublift/core/time.hpp>
#include <sublift/text/dedupe.hpp>

namespace {

sublift::SubtitleEntry e(std::int64_t s, std::int64_t en, const char* t, double c = 1.0) {
  return sublift::SubtitleEntry{
      .id = 0,
      .range = sublift::TimeRange{s, en},
      .text = t,
      .confidence = c,
  };
}

} // namespace

TEST_CASE("merge same adjacent", "[dedupe]") {
  const auto out = sublift::text::merge_entries(
      {e(0, 1000, "你好", 1.0), e(1100, 2000, "你好", 0.9)}, 1000, 0, false);
  REQUIRE(out.size() == 1);
  REQUIRE(out[0].range.start_ms == 0);
  REQUIRE(out[0].range.end_ms == 2000);
  REQUIRE(out[0].text == "你好");
  REQUIRE(out[0].confidence == 1.0);
}

TEST_CASE("no merge large gap", "[dedupe]") {
  const auto out = sublift::text::merge_entries(
      {e(0, 1000, "你好"), e(3000, 4000, "你好")}, 1000, 0, false);
  REQUIRE(out.size() == 2);
}

TEST_CASE("filter short", "[dedupe]") {
  const auto out = sublift::text::merge_entries(
      {e(0, 1000, "长"), e(1100, 1200, "短")}, 1000, 500, false);
  REQUIRE(out.size() == 1);
  REQUIRE(out[0].text == "长");
}

TEST_CASE("drop empty text", "[dedupe]") {
  const auto kept = sublift::text::merge_entries(
      {e(0, 1000, "有字"), e(1100, 2000, "  ")}, 1000, 0, false);
  REQUIRE(kept.size() == 2);
  const auto dropped = sublift::text::merge_entries(
      {e(0, 1000, "有字"), e(1100, 2000, "  ")}, 1000, 0, true);
  REQUIRE(dropped.size() == 1);
}

TEST_CASE("whitespace normalize merge", "[dedupe]") {
  const auto out = sublift::text::merge_entries(
      {e(0, 1000, "你好"), e(1000, 2000, " 你 好 ")}, 1000, 0, false);
  REQUIRE(out.size() == 1);
  REQUIRE(out[0].text == "你好");
}

TEST_CASE("empty between same text merges without drop_empty_text", "[dedupe]") {
  const auto out = sublift::text::merge_entries(
      {e(0, 4200, "放开我"), e(4200, 5000, ""), e(5000, 5800, "放开我")},
      1000, 500, false);
  REQUIRE(out.size() == 1);
  REQUIRE(out[0].range.start_ms == 0);
  REQUIRE(out[0].range.end_ms == 5800);
  REQUIRE(out[0].text == "放开我");
}

TEST_CASE("typewriter watermark fragments merge to longest", "[dedupe]") {
  const auto out = sublift::text::merge_entries(
      {e(0, 600, "更"), e(600, 1400, "多"), e(1400, 3200, "多资源尽在98堂"),
       e(3400, 5000, "更多资源尽在98堂"),
       e(5000, 6600, "更多资源尽在98堂永久地址："),
       e(6600, 7400, "更多资源尽在98堂永久地址：WWW.98T.LA"),
       e(8000, 8800, "更多资源尽在98堂永久地址："),
       e(8800, 9400, "更多资源尽在98堂")},
      1000, 0, false);
  REQUIRE(out.size() == 1);
  REQUIRE(out[0].range.start_ms == 0);
  REQUIRE(out[0].range.end_ms == 9400);
  REQUIRE(out[0].text.find("WWW.98T.LA") != std::string::npos);
}

TEST_CASE("single char not absorbed unless prefix", "[dedupe]") {
  const auto out = sublift::text::merge_entries(
      {e(0, 800, "嘛"), e(800, 2800, "知道啦进公司后就禁止")}, 1000, 0, false);
  REQUIRE(out.size() == 2);
}

TEST_CASE("OCR insertion variant merges to shorter text", "[dedupe]") {
  const auto out = sublift::text::merge_entries(
      {e(0, 1000, "哈茱蒂，本市第一位兔警员", 0.7),
       e(1000, 1800, "哈茱蒂，Y本市第一位兔警员", 0.9)},
      1000, 0, false);
  REQUIRE(out.size() == 1);
  REQUIRE(out[0].text == "哈茱蒂，本市第一位兔警员");
  REQUIRE(out[0].range.end_ms == 1800);
  REQUIRE(out[0].confidence == 0.9);
}

TEST_CASE("OCR substitution variant is not merged", "[dedupe]") {
  const auto out = sublift::text::merge_entries(
      {e(0, 1000, "第一组免狐搭档", 0.7),
       e(1000, 2000, "第一组兔狐搭档", 0.9)},
      1000, 0, false);
  REQUIRE(out.size() == 2);
}

TEST_CASE("containment keeps longer text over confidence", "[dedupe]") {
  const auto out = sublift::text::merge_entries(
      {e(0, 1000, "跟胡尼克", 0.99),
       e(1000, 2000, "跟胡尼克，一位平凡的狐狸", 0.80)},
      1000, 0, false);
  REQUIRE(out.size() == 1);
  REQUIRE(out[0].text == "跟胡尼克，一位平凡的狐狸");
  REQUIRE(out[0].confidence == 0.99);
}

TEST_CASE("similar variant does not merge across a large gap", "[dedupe]") {
  const auto out = sublift::text::merge_entries(
      {e(0, 1000, "哈茱蒂，本市第一位兔警员", 0.9),
       e(1500, 2500, "哈茱蒂，Y本市第一位兔警员", 0.7)},
      1000, 0, false);
  REQUIRE(out.size() == 2);
}

TEST_CASE("merge_entries extreme timestamps do not overflow or falsely merge", "[dedupe]") {
  constexpr auto kMax = std::numeric_limits<std::int64_t>::max();

  SECTION("negative end_ms and extreme start_ms do not falsely merge") {
    // (kMax - 50) - (-100) overflows signed int64 if unmitigated
    const auto out = sublift::text::merge_entries(
        {e(-500, -100, "第一段"), e(kMax - 50, kMax, "第一段")},
        1000, 0, false);
    REQUIRE(out.size() == 2);
    REQUIRE(out[0].range.start_ms == -500);
    REQUIRE(out[0].range.end_ms == -100);
    REQUIRE(out[1].range.start_ms == kMax - 50);
    REQUIRE(out[1].range.end_ms == kMax);
  }

  SECTION("inverted extreme duration is discarded cleanly in filter_short") {
    // -100 - (kMax - 50) overflows negative if unmitigated
    const auto out = sublift::text::merge_entries(
        {e(kMax - 50, -100, "倒置异常段")},
        1000, 500, false);
    REQUIRE(out.empty());
  }

  SECTION("extreme valid positive duration is retained in filter_short") {
    // (kMax - 50) - (-100) overflows positive if unmitigated
    const auto out = sublift::text::merge_entries(
        {e(-100, kMax - 50, "超长有效段")},
        1000, 500, false);
    REQUIRE(out.size() == 1);
    REQUIRE(out[0].range.start_ms == -100);
    REQUIRE(out[0].range.end_ms == kMax - 50);
  }
}
