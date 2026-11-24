#include <cmath>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

#include <catch2/catch_test_macros.hpp>
#include <golden_loader.hpp>
#include <sublift/core/models.hpp>
#include <sublift/text/cleaning.hpp>
#include <sublift/text/consensus.hpp>
#include <sublift/text/line_select.hpp>

TEST_CASE("line_select parity vs golden", "[parity][line_select]") {
  const auto project_root = std::filesystem::path{SUBLIFT_PROJECT_SOURCE_DIR};
  const auto golden_path = project_root / "benchmark/parity/goldens/line_select/line_select.v1.json";

  auto root = sublift::test::load_golden_json(golden_path);

  REQUIRE(root["golden_schema_version"].get<int>() == 1);
  REQUIRE(root["kind"].get<std::string>() == "line_select");
  const auto& cases = root["cases"];
  REQUIRE_FALSE(cases.empty());

  for (const auto& c : cases) {
    const std::string name = c["name"].get<std::string>();
    const std::string kind = c["kind"].get<std::string>();
    DYNAMIC_SECTION("Case: " << name) {
      if (kind == "normalize") {
        const auto got = sublift::text::normalize_ocr_text(c["input"].get<std::string>());
        const auto exp = c["output"].get<std::string>();
        REQUIRE(got == exp);
      } else if (kind == "cleanup") {
        const auto got = sublift::text::cleanup_subtitle_text(
            c["input"].get<std::string>(), c["script"].get<std::string>());
        const auto exp = c["output"].get<std::string>();
        REQUIRE(got == exp);
      } else if (kind == "edit_distance") {
        REQUIRE(sublift::text::edit_distance(c["a"].get<std::string>(), c["b"].get<std::string>()) ==
                c["distance"].get<std::int64_t>());
      } else if (kind == "select_line") {
        sublift::SubtitleProfile profile;
        const auto& p = c["profile"];
        profile.script = p["script"].get<std::string>();
        profile.center_x = p["center_x"].get<std::int32_t>();
        profile.center_y = p["center_y"].get<std::int32_t>();
        profile.height = p["height"].get<std::int32_t>();
        profile.y_min = p["y_min"].get<std::int32_t>();
        profile.y_max = p["y_max"].get<std::int32_t>();

        std::vector<sublift::OcrLine> lines;
        for (const auto& ln : c["lines"]) {
          const auto& box = ln["box"];
          lines.push_back(sublift::OcrLine{
              .text = ln["text"].get<std::string>(),
              .confidence = ln["confidence"].get<double>(),
              .box = sublift::OcrCropBox{
                  box["x"].get<std::int32_t>(),
                  box["y"].get<std::int32_t>(),
                  box["width"].get<std::int32_t>(),
                  box["height"].get<std::int32_t>(),
              },
          });
        }
        const auto sel = sublift::text::select_line(lines, profile);
        if (c["selected_text"].is_null()) {
          REQUIRE_FALSE(sel.has_value());
        } else {
          REQUIRE(sel.has_value());
          REQUIRE(sel->text == c["selected_text"].get<std::string>());
        }
      } else if (kind == "consensus") {
        std::vector<std::pair<std::string, double>> samples;
        for (const auto& s : c["samples"]) {
          samples.emplace_back(s[0].get<std::string>(), s[1].get<double>());
        }
        const auto r = sublift::text::consensus_text(samples, c["script"].get<std::string>());
        const auto& exp = c["result"];
        REQUIRE(r.text == exp["text"].get<std::string>());
        REQUIRE(r.support_votes == exp["support_votes"].get<std::int64_t>());
        REQUIRE(std::abs(r.confidence - exp["confidence"].get<double>()) < 1e-9);
      } else if (kind == "should_accept") {
        sublift::SubtitleProfile profile;
        profile.script = "cjk";
        const bool acc = sublift::text::should_accept_text(
            c["text"].get<std::string>(), c["confidence"].get<double>(), profile, 0.5, 0.28, 2);
        REQUIRE(acc == c["accept"].get<bool>());
      }
    }
  }
}
