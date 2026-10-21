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
