#include <catch2/catch_test_macros.hpp>
#include <chrono>
#include <cstdint>
#include <limits>
#include <random>
#include <string>
#include <vector>

#include <sublift/core/subtitle.hpp>
#include <sublift/core/time.hpp>
#include <sublift/text/cleaning.hpp>
#include <sublift/text/dedupe.hpp>
#include <sublift/text/srt.hpp>

TEST_CASE("merge_entries: Empty input handling under various parameters",
          "[dedupe][adversarial][empty]") {
  SECTION("Default arguments") {
    auto res = sublift::text::merge_entries({});
    REQUIRE(res.empty());
  }
  SECTION("Explicit standard parameters") {
    auto res = sublift::text::merge_entries({}, 1000, 500, false);
    REQUIRE(res.empty());
  }
  SECTION("drop_empty_text true") {
    auto res = sublift::text::merge_entries({}, 1000, 500, true);
    REQUIRE(res.empty());
  }
  SECTION("Zero gap and zero min duration") {
    auto res = sublift::text::merge_entries({}, 0, 0, false);
    REQUIRE(res.empty());
  }
  SECTION("Negative gap and negative min duration") {
    auto res = sublift::text::merge_entries({}, -1000, -500, true);
    REQUIRE(res.empty());
  }
  SECTION("Extreme gap and duration") {
    auto res = sublift::text::merge_entries(
        {}, std::numeric_limits<std::int64_t>::max(),
        std::numeric_limits<std::int64_t>::max(), false);
    REQUIRE(res.empty());
  }
}

TEST_CASE("merge_entries: Negative durations and negative timestamps",
          "[dedupe][adversarial][negative]") {
  SECTION("Single negative duration entry (start_ms > end_ms) with standard min_duration_ms") {
    auto res = sublift::text::merge_entries(
        {sublift::SubtitleEntry{.id = 0, .range = {1000, 500}, .text = "NegativeDuration"}},
        1000, 500, false);
    REQUIRE(res.empty());
  }
  SECTION("Single negative duration entry with min_duration_ms = 0") {
    auto res = sublift::text::merge_entries(
        {sublift::SubtitleEntry{.id = 0, .range = {1000, 500}, .text = "NegativeDuration"}},
        1000, 0, false);
    REQUIRE(res.empty());
  }
  SECTION("Negative duration entry with negative min_duration_ms") {
    auto res = sublift::text::merge_entries(
        {sublift::SubtitleEntry{.id = 0, .range = {1000, 500}, .text = "NegativeDuration"}},
        1000, -1000, false);
    REQUIRE(res.size() == 1);
    REQUIRE(res[0].range.start_ms == 1000);
    REQUIRE(res[0].range.end_ms == 500);
    REQUIRE(res[0].id == 1);
  }
  SECTION("Adjacent identical entries with negative durations merging in Pass 1") {
    auto res = sublift::text::merge_entries(
        {sublift::SubtitleEntry{.id = 0, .range = {1000, 500}, .text = "Inverted"},
         sublift::SubtitleEntry{.id = 0, .range = {600, 200}, .text = "Inverted"}},
        1000, -2000, false);
    REQUIRE(res.size() == 1);
    REQUIRE(res[0].range.start_ms == 1000);
    REQUIRE(res[0].range.end_ms == 200);
    REQUIRE(res[0].text == "Inverted");
  }
  SECTION("Negative timestamps (start_ms < 0, end_ms < 0)") {
    auto res = sublift::text::merge_entries(
        {sublift::SubtitleEntry{.id = 0, .range = {-2000, -1000}, .text = "NegativeTime"},
         sublift::SubtitleEntry{.id = 0, .range = {-800, -200}, .text = "NegativeTime"}},
        1000, 500, false);
    REQUIRE(res.size() == 1);
    REQUIRE(res[0].range.start_ms == -2000);
    REQUIRE(res[0].range.end_ms == -200);
    REQUIRE(res[0].text == "NegativeTime");
  }
  SECTION("Negative start_ms spanning past zero") {
    auto res = sublift::text::merge_entries(
        {sublift::SubtitleEntry{.id = 0, .range = {-500, 1000}, .text = "SpanningZero"}},
        1000, 500, false);
    REQUIRE(res.size() == 1);
    REQUIRE(res[0].range.start_ms == -500);
    REQUIRE(res[0].range.end_ms == 1000);
  }
}

TEST_CASE("merge_entries: Extreme timestamps near INT64_MAX",
          "[dedupe][adversarial][extreme]") {
  constexpr auto kMax = std::numeric_limits<std::int64_t>::max();

  SECTION("Single entry at INT64_MAX") {
    auto res = sublift::text::merge_entries(
        {sublift::SubtitleEntry{.id = 0, .range = {kMax - 1000, kMax}, .text = "MaxBoundary"}},
        1000, 500, false);
    REQUIRE(res.size() == 1);
    REQUIRE(res[0].range.start_ms == kMax - 1000);
    REQUIRE(res[0].range.end_ms == kMax);
  }
  SECTION("Adjacent identical entries near INT64_MAX merging without overflow") {
    auto res = sublift::text::merge_entries(
        {sublift::SubtitleEntry{.id = 0, .range = {kMax - 3000, kMax - 2000}, .text = "ExtremeMerge"},
         sublift::SubtitleEntry{.id = 0, .range = {kMax - 1500, kMax}, .text = "ExtremeMerge"}},
        1000, 500, false);
    REQUIRE(res.size() == 1);
    REQUIRE(res[0].range.start_ms == kMax - 3000);
    REQUIRE(res[0].range.end_ms == kMax);
  }
  SECTION("Extreme gap from 0 to INT64_MAX does not merge") {
    auto res = sublift::text::merge_entries(
        {sublift::SubtitleEntry{.id = 1, .range = {0, 1000}, .text = "First"},
         sublift::SubtitleEntry{.id = 2, .range = {kMax - 1000, kMax}, .text = "Second"}},
        1000, 500, false);
    REQUIRE(res.size() == 2);
    REQUIRE(res[0].id == 1);
    REQUIRE(res[1].id == 2);
    REQUIRE(res[1].range.end_ms == kMax);
  }
  SECTION("Cross-zero extreme gap with negative timestamp does not overflow or falsely merge") {
    auto res = sublift::text::merge_entries(
        {sublift::SubtitleEntry{.id = 1, .range = {-500, -100}, .text = "First"},
         sublift::SubtitleEntry{.id = 2, .range = {kMax - 50, kMax}, .text = "First"}},
        1000, 0, false);
    REQUIRE(res.size() == 2);
    REQUIRE(res[0].range.start_ms == -500);
    REQUIRE(res[0].range.end_ms == -100);
    REQUIRE(res[1].range.start_ms == kMax - 50);
    REQUIRE(res[1].range.end_ms == kMax);
  }
}

TEST_CASE("merge_entries: Exact gap boundary condition (gap == merge_gap_ms vs gap == merge_gap_ms + 1)",
          "[dedupe][adversarial][gap]") {
  SECTION("gap == merge_gap_ms: MUST MERGE") {
    auto res = sublift::text::merge_entries(
        {sublift::SubtitleEntry{.id = 0, .range = {0, 1000}, .text = "ExactGap"},
         sublift::SubtitleEntry{.id = 0, .range = {2000, 3000}, .text = "ExactGap"}},
        1000, 500, false);
    REQUIRE(res.size() == 1);
    REQUIRE(res[0].range.start_ms == 0);
    REQUIRE(res[0].range.end_ms == 3000);
    REQUIRE(res[0].text == "ExactGap");
  }
  SECTION("gap == merge_gap_ms + 1: MUST NOT MERGE") {
    auto res = sublift::text::merge_entries(
        {sublift::SubtitleEntry{.id = 0, .range = {0, 1000}, .text = "ExactGap"},
         sublift::SubtitleEntry{.id = 0, .range = {2001, 3001}, .text = "ExactGap"}},
        1000, 500, false);
    REQUIRE(res.size() == 2);
    REQUIRE(res[0].range.end_ms == 1000);
    REQUIRE(res[1].range.start_ms == 2001);
  }
  SECTION("gap == 0 (abutting entries): MUST MERGE") {
    auto res = sublift::text::merge_entries(
        {sublift::SubtitleEntry{.id = 0, .range = {0, 1000}, .text = "Abutting"},
         sublift::SubtitleEntry{.id = 0, .range = {1000, 2000}, .text = "Abutting"}},
        1000, 500, false);
    REQUIRE(res.size() == 1);
    REQUIRE(res[0].range.start_ms == 0);
    REQUIRE(res[0].range.end_ms == 2000);
  }
  SECTION("gap < 0 (overlapping entries): MUST MERGE") {
    auto res = sublift::text::merge_entries(
        {sublift::SubtitleEntry{.id = 0, .range = {0, 1500}, .text = "Overlapping"},
         sublift::SubtitleEntry{.id = 0, .range = {1000, 2500}, .text = "Overlapping"}},
        1000, 500, false);
    REQUIRE(res.size() == 1);
    REQUIRE(res[0].range.start_ms == 0);
    REQUIRE(res[0].range.end_ms == 2500);
  }
}

TEST_CASE("merge_entries: Interspersed empty entries and cross-empty backward scan",
          "[dedupe][adversarial][empty_scan]") {
  SECTION("Multiple empty entries between identical text within merge_gap_ms") {
    auto res = sublift::text::merge_entries(
        {sublift::SubtitleEntry{.id = 0, .range = {0, 1000}, .text = "Target"},
         sublift::SubtitleEntry{.id = 0, .range = {1200, 1400}, .text = ""},
         sublift::SubtitleEntry{.id = 0, .range = {1500, 1700}, .text = "   "},
         sublift::SubtitleEntry{.id = 0, .range = {1900, 3000}, .text = "Target"}},
        1000, 500, false);
    REQUIRE(res.size() == 1);
    REQUIRE(res[0].range.start_ms == 0);
    REQUIRE(res[0].range.end_ms == 3000);
    REQUIRE(res[0].text == "Target");
  }
  SECTION("Empty entries between identical text exceeding merge_gap_ms with drop_empty_text=false") {
    auto res = sublift::text::merge_entries(
        {sublift::SubtitleEntry{.id = 0, .range = {0, 1000}, .text = "Target"},
         sublift::SubtitleEntry{.id = 0, .range = {1200, 1400}, .text = ""},
         sublift::SubtitleEntry{.id = 0, .range = {2500, 3500}, .text = "Target"}},
        1000, 0, false);
    REQUIRE(res.size() == 3);
  }
  SECTION("Empty entries between identical text exceeding merge_gap_ms with drop_empty_text=true") {
    auto res = sublift::text::merge_entries(
        {sublift::SubtitleEntry{.id = 0, .range = {0, 1000}, .text = "Target"},
         sublift::SubtitleEntry{.id = 0, .range = {1200, 1400}, .text = ""},
         sublift::SubtitleEntry{.id = 0, .range = {2500, 3500}, .text = "Target"}},
        1000, 0, true);
    REQUIRE(res.size() == 2);
  }
}

TEST_CASE("merge_entries: kContainment vs kSimilar merge semantics",
          "[dedupe][adversarial][merge_kind]") {
  SECTION("kContainment retains longer text regardless of arrival order") {
    auto res1 = sublift::text::merge_entries(
        {sublift::SubtitleEntry{.id = 0, .range = {0, 1000}, .text = "深入理解", .confidence = 0.99},
         sublift::SubtitleEntry{.id = 0, .range = {1000, 2000}, .text = "深入理解计算机系统", .confidence = 0.80}},
        1000, 500, false);
    REQUIRE(res1.size() == 1);
    REQUIRE(res1[0].text == "深入理解计算机系统");
    REQUIRE(res1[0].confidence == 0.99);

    auto res2 = sublift::text::merge_entries(
        {sublift::SubtitleEntry{.id = 0, .range = {0, 1000}, .text = "深入理解计算机系统", .confidence = 0.80},
         sublift::SubtitleEntry{.id = 0, .range = {1000, 2000}, .text = "深入理解", .confidence = 0.99}},
        1000, 500, false);
    REQUIRE(res2.size() == 1);
    REQUIRE(res2[0].text == "深入理解计算机系统");
    REQUIRE(res2[0].confidence == 0.99);
  }
  SECTION("kSimilar retains shorter clean text regardless of arrival order") {
    auto res1 = sublift::text::merge_entries(
        {sublift::SubtitleEntry{.id = 0, .range = {0, 1000}, .text = "哈茱蒂，本市第一位兔警员", .confidence = 0.70},
         sublift::SubtitleEntry{.id = 0, .range = {1000, 1800}, .text = "哈茱蒂，Y本市第一位兔警员", .confidence = 0.90}},
        1000, 500, false);
    REQUIRE(res1.size() == 1);
    REQUIRE(res1[0].text == "哈茱蒂，本市第一位兔警员");
    REQUIRE(res1[0].confidence == 0.90);

    auto res2 = sublift::text::merge_entries(
        {sublift::SubtitleEntry{.id = 0, .range = {0, 1000}, .text = "哈茱蒂，Y本市第一位兔警员", .confidence = 0.90},
         sublift::SubtitleEntry{.id = 0, .range = {1000, 1800}, .text = "哈茱蒂，本市第一位兔警员", .confidence = 0.70}},
        1000, 500, false);
    REQUIRE(res2.size() == 1);
    REQUIRE(res2[0].text == "哈茱蒂，本市第一位兔警员");
    REQUIRE(res2[0].confidence == 0.90);
  }
  SECTION("kSimilar gap threshold constraint (kSimilarityMaxGapMs = 300)") {
    auto res_merge = sublift::text::merge_entries(
        {sublift::SubtitleEntry{.id = 0, .range = {0, 1000}, .text = "哈茱蒂，本市第一位兔警员"},
         sublift::SubtitleEntry{.id = 0, .range = {1300, 2300}, .text = "哈茱蒂，Y本市第一位兔警员"}},
        1000, 500, false);
    REQUIRE(res_merge.size() == 1);
    REQUIRE(res_merge[0].text == "哈茱蒂，本市第一位兔警员");

    auto res_no_merge = sublift::text::merge_entries(
        {sublift::SubtitleEntry{.id = 0, .range = {0, 1000}, .text = "哈茱蒂，本市第一位兔警员"},
         sublift::SubtitleEntry{.id = 0, .range = {1301, 2301}, .text = "哈茱蒂，Y本市第一位兔警员"}},
        1000, 500, false);
    REQUIRE(res_no_merge.size() == 2);
  }
}

TEST_CASE("merge_entries: Pass 4 secondary re-merge after Pass 3 short-segment filtering",
          "[dedupe][adversarial][pass4]") {
  auto res = sublift::text::merge_entries(
      {sublift::SubtitleEntry{.id = 0, .range = {0, 1000}, .text = "CommonSubtitle"},
       sublift::SubtitleEntry{.id = 0, .range = {1100, 1200}, .text = "NoiseGlitch"},
       sublift::SubtitleEntry{.id = 0, .range = {1400, 2400}, .text = "CommonSubtitle"}},
      1000, 500, false);
  REQUIRE(res.size() == 1);
  REQUIRE(res[0].range.start_ms == 0);
  REQUIRE(res[0].range.end_ms == 2400);
  REQUIRE(res[0].text == "CommonSubtitle");
}

TEST_CASE("merge_entries: Sequential 1-based ID re-indexing",
          "[dedupe][adversarial][ids]") {
  auto res = sublift::text::merge_entries(
      {sublift::SubtitleEntry{.id = 100, .range = {0, 1000}, .text = "A"},
       sublift::SubtitleEntry{.id = 500, .range = {2000, 3000}, .text = "B"},
       sublift::SubtitleEntry{.id = 0, .range = {4000, 5000}, .text = "C"},
       sublift::SubtitleEntry{.id = 99999, .range = {6000, 7000}, .text = "D"}},
      500, 500, false);
  REQUIRE(res.size() == 4);
  for (std::size_t i = 0; i < res.size(); ++i) {
    REQUIRE(res[i].id == static_cast<std::uint64_t>(i + 1));
  }
}

TEST_CASE("format_timestamp_srt: Timestamp boundary conditions",
          "[srt][adversarial][timestamp]") {
  REQUIRE(sublift::text::format_timestamp_srt(0) == "00:00:00,000");
  REQUIRE(sublift::text::format_timestamp_srt(1) == "00:00:00,001");
  REQUIRE(sublift::text::format_timestamp_srt(999) == "00:00:00,999");
  REQUIRE(sublift::text::format_timestamp_srt(1000) == "00:00:01,000");
  REQUIRE(sublift::text::format_timestamp_srt(59'999) == "00:00:59,999");
  REQUIRE(sublift::text::format_timestamp_srt(60'000) == "00:01:00,000");
  REQUIRE(sublift::text::format_timestamp_srt(3'599'999) == "00:59:59,999");
  REQUIRE(sublift::text::format_timestamp_srt(3'600'000) == "01:00:00,000");
  REQUIRE(sublift::text::format_timestamp_srt(3'596'400'000LL) == "999:00:00,000");
  REQUIRE(sublift::text::format_timestamp_srt(-1) == "00:00:00,000");
  REQUIRE(sublift::text::format_timestamp_srt(-999'999) == "00:00:00,000");
  REQUIRE(sublift::text::format_timestamp_srt(std::numeric_limits<std::int64_t>::min()) == "00:00:00,000");

  const auto str = sublift::text::format_timestamp_srt(std::numeric_limits<std::int64_t>::max() - 100);
  REQUIRE(!str.empty());
  REQUIRE(str.find(",") != std::string::npos);
}

TEST_CASE("format_srt: Cue formatting, numbering, whitespace skipping, and LF line endings",
          "[srt][adversarial][formatting]") {
  SECTION("Empty input produces empty string") {
    REQUIRE(sublift::text::format_srt({}) == "");
  }
  SECTION("Entries with only blank text are skipped and cues remain strictly sequential") {
    std::vector<sublift::SubtitleEntry> entries{
        sublift::SubtitleEntry{.id = 0, .range = {1000, 2000}, .text = "   "},
        sublift::SubtitleEntry{.id = 0, .range = {3000, 4000}, .text = "First Valid Line"},
        sublift::SubtitleEntry{.id = 0, .range = {4500, 5000}, .text = "\t\n\r"},
        sublift::SubtitleEntry{.id = 0, .range = {6000, 7000}, .text = "Second Valid Line"},
    };
    const auto [srt, count] = sublift::text::format_srt_counted(entries);
    REQUIRE(count == 2);
    REQUIRE(srt.find('\r') == std::string::npos);

    const std::string expected =
        "1\n00:00:03,000 --> 00:00:04,000\nFirst Valid Line\n\n"
        "2\n00:00:06,000 --> 00:00:07,000\nSecond Valid Line\n\n";
    REQUIRE(srt == expected);
  }
  SECTION("Negative and inverted timestamps in format_srt") {
    std::vector<sublift::SubtitleEntry> entries{
        sublift::SubtitleEntry{.id = 0, .range = {-1000, 1000}, .text = "NegativeStart"},
        sublift::SubtitleEntry{.id = 0, .range = {2000, 1000}, .text = "Inverted"},
    };
    const auto srt = sublift::text::format_srt(entries);
    const std::string expected =
        "1\n00:00:00,000 --> 00:00:01,000\nNegativeStart\n\n"
        "2\n00:00:02,000 --> 00:00:02,000\nInverted\n\n";
    REQUIRE(srt == expected);
  }
}

TEST_CASE("merge_entries: Massive sequence stress (15,000 entries)",
          "[dedupe][adversarial][massive]") {
  std::mt19937_64 rng(1337);
  std::vector<sublift::SubtitleEntry> entries;
  entries.reserve(15000);

  const std::vector<std::string> sample_texts = {
      "今日头条新闻播报",
      "今日头条新闻播报",
      "更多精彩请关注",
      "更多精彩请关注官方频道",
      "短噪",
      "",
  };

  std::int64_t current_time = 0;
  for (int i = 0; i < 15000; ++i) {
    const auto& t = sample_texts[rng() % sample_texts.size()];
    const std::int64_t dur = (t == "短噪") ? 100 : (600 + (rng() % 1000));
    entries.push_back(sublift::SubtitleEntry{
        .id = static_cast<std::uint64_t>(i),
        .range = {current_time, current_time + dur},
        .text = t,
        .confidence = 0.85,
    });
    current_time += dur + (rng() % 300);
  }

  const auto result = sublift::text::merge_entries(entries, 1000, 500, false);
  REQUIRE(!result.empty());

  for (std::size_t i = 0; i < result.size(); ++i) {
    REQUIRE(result[i].id == static_cast<std::uint64_t>(i + 1));
    if (!result[i].text.empty()) {
      REQUIRE(result[i].range.end_ms - result[i].range.start_ms >= 500);
    }
  }

  const auto srt_res = sublift::text::format_srt_counted(result);
  REQUIRE(!srt_res.first.empty());
}

TEST_CASE("merge_entries: Mathematical idempotence property",
          "[dedupe][adversarial][idempotent]") {
  std::mt19937_64 rng(42);
  std::vector<sublift::SubtitleEntry> entries;
  std::int64_t t = 0;
  for (int i = 0; i < 200; ++i) {
    std::int64_t dur = 200 + (rng() % 1200);
    entries.push_back(sublift::SubtitleEntry{
        .id = 0,
        .range = {t, t + dur},
        .text = ((i % 3 == 0) ? "固定重复内容" : ("动态条目 " + std::to_string(i))),
        .confidence = 0.9,
    });
    t += dur + (rng() % 500);
  }

  auto pass1 = sublift::text::merge_entries(entries, 1000, 500, false);
  auto pass2 = sublift::text::merge_entries(pass1, 1000, 500, false);

  REQUIRE(pass1.size() == pass2.size());
  for (std::size_t i = 0; i < pass1.size(); ++i) {
    REQUIRE(pass1[i].id == pass2[i].id);
    REQUIRE(pass1[i].range.start_ms == pass2[i].range.start_ms);
    REQUIRE(pass1[i].range.end_ms == pass2[i].range.end_ms);
    REQUIRE(pass1[i].text == pass2[i].text);
  }
}

TEST_CASE("srt: is_blank_text vs cleaning::normalize_whitespace Unicode awareness",
          "[srt][cleaning][adversarial][whitespace]") {
  SECTION("ASCII whitespace strings") {
    REQUIRE(sublift::text::normalize_whitespace(" \t\n\r ").empty());
  }
  SECTION("Full-width ideographic space U+3000") {
    REQUIRE(sublift::text::normalize_whitespace("\xE3\x80\x80").empty());
    REQUIRE(sublift::text::normalize_whitespace("  \xE3\x80\x80  ").empty());
  }
  SECTION("is_blank_text with Unicode whitespace") {
    REQUIRE(sublift::text::is_blank_text("\xE3\x80\x80"));
    REQUIRE(sublift::text::is_blank_text("\u3000"));
    REQUIRE(sublift::text::is_blank_text("\u00A0"));
    REQUIRE_FALSE(sublift::text::is_blank_text("有效字幕"));
  }
}
