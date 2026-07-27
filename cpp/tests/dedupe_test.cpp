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
