#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <golden_loader.hpp>
#include <sublift/core/subtitle.hpp>
#include <sublift/text/dedupe.hpp>

TEST_CASE("dedupe parity vs golden (dedupe.v1.json)", "[parity][text][dedupe]") {
  const auto project_root = std::filesystem::path{SUBLIFT_PROJECT_SOURCE_DIR};
  const auto golden_path = project_root / "benchmark/parity/goldens/entries/dedupe.v1.json";

  auto root = sublift::test::load_golden_json(golden_path);

  REQUIRE(root["golden_schema_version"].get<int>() == 1);
  REQUIRE(root["kind"].get<std::string>() == "dedupe");
  const auto& scenarios = root["scenarios"];
  REQUIRE_FALSE(scenarios.empty());

  for (const auto& sc : scenarios) {
    const std::string name = sc["name"].get<std::string>();
    DYNAMIC_SECTION("Scenario: " << name) {
      std::vector<sublift::SubtitleEntry> entries;
      for (const auto& e : sc["entries"]) {
        entries.push_back(sublift::SubtitleEntry{
            .id = 0,
            .range = sublift::TimeRange{e["start_ms"].get<std::int64_t>(), e["end_ms"].get<std::int64_t>()},
            .text = e["text"].get<std::string>(),
            .confidence = e["confidence"].get<double>(),
        });
      }

      const auto out = sublift::text::merge_entries(
          entries,
          sc["merge_gap_ms"].get<std::int64_t>(),
          sc["min_duration_ms"].get<std::int64_t>(),
          sc["drop_empty_text"].get<bool>());

      const auto& exp = sc["result"];
      REQUIRE(out.size() == exp.size());
      for (std::size_t i = 0; i < out.size(); ++i) {
        REQUIRE(out[i].range.start_ms == exp[i]["start_ms"].get<std::int64_t>());
        REQUIRE(out[i].range.end_ms == exp[i]["end_ms"].get<std::int64_t>());
        REQUIRE(out[i].text == exp[i]["text"].get<std::string>());
        REQUIRE(out[i].confidence == exp[i]["confidence"].get<double>());
      }
    }
  }
}
