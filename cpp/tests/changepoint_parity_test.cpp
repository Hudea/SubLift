#include <catch2/catch_test_macros.hpp>
#include <filesystem>
#include <string>
#include <unordered_map>
#include <vector>

#include <golden_loader.hpp>
#include <sublift/core/color.hpp>
#include <sublift/core/image_buffer.hpp>
#include <sublift/core/image_view.hpp>
#include <sublift/timing/changepoint.hpp>

TEST_CASE("changepoint parity vs frozen golden (changepoint.v1.json)", "[parity][timing][changepoint]") {
  const auto project_root = std::filesystem::path{SUBLIFT_PROJECT_SOURCE_DIR};
  const auto golden_path = project_root / "benchmark/parity/goldens/events/changepoint.v1.json";
  const auto fixtures_dir = project_root / "benchmark/parity/fixtures/changepoint";

  auto root = sublift::test::load_golden_json(golden_path);

  REQUIRE(root["golden_schema_version"].get<int>() == 1);
  const auto& scenarios = root["scenarios"];
  REQUIRE_FALSE(scenarios.empty());

  // Load crop fixtures
  std::unordered_map<std::string, sublift::ImageBuffer> crops;
  for (const auto& c : root["crops"]) {
    std::string name = c["name"].get<std::string>();
    std::string asset = c["asset"].get<std::string>();
    int width = c["width"].get<int>();
    int height = c["height"].get<int>();
    auto crop_file = fixtures_dir / asset;
    crops.emplace(name, sublift::test::load_crop_rgb(crop_file, width, height));
  }

  for (const auto& sc : scenarios) {
    std::string sc_name = sc["name"].get<std::string>();
    DYNAMIC_SECTION("Scenario: " << sc_name) {
      const auto& sc_cfg = sc["change_point_config"];
      sublift::timing::TimingConfig cfg = sublift::test::parse_timing_config(sc_cfg);

      sublift::timing::ChangePointDetector det(cfg);
      std::vector<sublift::timing::StateEvent> actual_events;

      for (const auto& fr : sc["frames"]) {
        sublift::features::FrameSignature sig{
            .timestamp_ms = fr["timestamp_ms"].get<std::int64_t>(),
            .foreground_ratio = fr["foreground_ratio"].get<double>(),
            .dhash = sublift::test::get_uint64(fr["dhash"]),
        };

        sublift::ImageView crop_view{};
        if (fr.contains("crop") && fr["crop"].is_string() && !fr["crop"].get<std::string>().empty()) {
          auto it = crops.find(fr["crop"].get<std::string>());
          REQUIRE(it != crops.end());
          crop_view = it->second.view();
        }

        auto ev = det.process(sig, crop_view);
        if (ev.has_value()) {
          actual_events.push_back(*ev);
        }
      }

      const auto& expected_events = sc["events"];
      INFO("Scenario " << sc_name << " event count mismatch: actual " << actual_events.size()
                       << " vs expected " << expected_events.size());
      REQUIRE(actual_events.size() == expected_events.size());

      for (std::size_t i = 0; i < actual_events.size(); ++i) {
        const auto& actual = actual_events[i];
        const auto& expected = expected_events[i];

        std::string act_type = sublift::test::event_type_to_string(actual.type);
        std::string exp_type = expected["event_type"].get<std::string>();
        INFO("Event " << i << " type mismatch: actual " << act_type << " vs expected " << exp_type);
        REQUIRE(act_type == exp_type);

        INFO("Event " << i << " timestamp mismatch: actual " << actual.timestamp_ms
                      << " vs expected " << expected["timestamp_ms"].get<std::int64_t>());
        REQUIRE(actual.timestamp_ms == expected["timestamp_ms"].get<std::int64_t>());

        if (expected.contains("prev_end_ms") && expected["prev_end_ms"].is_number()) {
          REQUIRE(actual.prev_end_ms.has_value());
          REQUIRE(*actual.prev_end_ms == expected["prev_end_ms"].get<std::int64_t>());
        } else {
          REQUIRE_FALSE(actual.prev_end_ms.has_value());
        }
      }
    }
  }
}
