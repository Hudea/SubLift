#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <golden_loader.hpp>
#include <sublift/timing/timeline.hpp>

TEST_CASE("timeline parity vs golden (timeline.v1.json)", "[parity][timing][timeline]") {
  const auto project_root = std::filesystem::path{SUBLIFT_PROJECT_SOURCE_DIR};
  const auto golden_path = project_root / "benchmark/parity/goldens/segments/timeline.v1.json";

  auto root = sublift::test::load_golden_json(golden_path);

  REQUIRE(root["golden_schema_version"].get<int>() == 1);
  const auto& scenarios = root["scenarios"];
  REQUIRE_FALSE(scenarios.empty());

  for (const auto& sc : scenarios) {
    std::string sc_name = sc["name"].get<std::string>();
    DYNAMIC_SECTION("Scenario: " << sc_name) {
      sublift::timing::TimelineBuilder builder;

      for (const auto& ev_json : sc["events"]) {
        sublift::timing::EventType et = sublift::test::string_to_event_type(ev_json["event_type"].get<std::string>());

        std::optional<std::int64_t> prev_end = std::nullopt;
        if (ev_json.contains("prev_end_ms") && ev_json["prev_end_ms"].is_number()) {
          prev_end = ev_json["prev_end_ms"].get<std::int64_t>();
        }

        builder.consume(sublift::timing::StateEvent{
            .type = et,
            .timestamp_ms = ev_json["timestamp_ms"].get<std::int64_t>(),
            .prev_end_ms = prev_end,
        });
      }

      if (sc.contains("finalize_ms") && sc["finalize_ms"].is_number()) {
        builder.finalize(sc["finalize_ms"].get<std::int64_t>());
      }

      const auto& segs = builder.segments();
      const auto& expected_segs = sc["segments"];

      INFO("Scenario " << sc_name << " segment count mismatch: actual " << segs.size()
                       << " vs expected " << expected_segs.size());
      REQUIRE(segs.size() == expected_segs.size());

      for (std::size_t i = 0; i < segs.size(); ++i) {
        INFO("Segment " << i << " start_ms mismatch: actual " << segs[i].start_ms
                        << " vs expected " << expected_segs[i]["start_ms"].get<std::int64_t>());
        REQUIRE(segs[i].start_ms == expected_segs[i]["start_ms"].get<std::int64_t>());

        INFO("Segment " << i << " end_ms mismatch: actual " << segs[i].end_ms
                        << " vs expected " << expected_segs[i]["end_ms"].get<std::int64_t>());
        REQUIRE(segs[i].end_ms == expected_segs[i]["end_ms"].get<std::int64_t>());
      }
    }
  }
}
