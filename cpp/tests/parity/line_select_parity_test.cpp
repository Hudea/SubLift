#include <catch2/catch_test_macros.hpp>

#include "sublift/line_select.hpp"
#include "sublift/test_support.hpp"

#include <cmath>
#include <fstream>
#include <nlohmann/json.hpp>

#ifndef SUBLIFT_PARITY_GOLDEN_LINE_SELECT
#error "SUBLIFT_PARITY_GOLDEN_LINE_SELECT required"
#endif

TEST_CASE("line_select parity vs golden", "[parity][line_select]") {
  std::ifstream in(SUBLIFT_PARITY_GOLDEN_LINE_SELECT);
  REQUIRE(in);
  nlohmann::json root;
  in >> root;
  REQUIRE(root.at("kind") == "line_select");
  for (const auto& c : root.at("cases")) {
    const auto kind = c.at("kind").get<std::string>();
    INFO("case " << c.at("name").get<std::string>());
    if (kind == "normalize") {
      const auto got = sublift::test_support::utf8_nfc(
          sublift::normalize_ocr_text(c.at("input").get<std::string>()));
      const auto exp = sublift::test_support::utf8_nfc(
          c.at("output").get<std::string>());
      REQUIRE(got == exp);
    } else if (kind == "cleanup") {
      const auto got = sublift::test_support::utf8_nfc(
          sublift::cleanup_subtitle_text(c.at("input").get<std::string>(),
                                         c.at("script").get<std::string>()));
      const auto exp = sublift::test_support::utf8_nfc(
          c.at("output").get<std::string>());
      REQUIRE(got == exp);
    } else if (kind == "edit_distance") {
      REQUIRE(sublift::edit_distance(c.at("a").get<std::string>(),
                                     c.at("b").get<std::string>()) ==
              c.at("distance").get<int>());
    } else if (kind == "select_line") {
      sublift::SubtitleProfile profile;
      const auto& p = c.at("profile");
      profile.script = p.at("script").get<std::string>();
      profile.center_x = p.at("center_x").get<std::int32_t>();
      profile.center_y = p.at("center_y").get<std::int32_t>();
      profile.height = p.at("height").get<std::int32_t>();
      profile.y_min = p.at("y_min").get<std::int32_t>();
      profile.y_max = p.at("y_max").get<std::int32_t>();
      std::vector<sublift::OcrLine> lines;
      for (const auto& ln : c.at("lines")) {
        const auto& box = ln.at("box");
        lines.push_back(sublift::OcrLine{
            ln.at("text").get<std::string>(),
            ln.at("confidence").get<double>(),
            {box.at("x").get<std::int32_t>(), box.at("y").get<std::int32_t>(),
             box.at("width").get<std::int32_t>(),
             box.at("height").get<std::int32_t>()},
        });
      }
      const auto sel = sublift::select_line(lines, profile);
      if (c.at("selected_text").is_null()) {
        REQUIRE_FALSE(sel.has_value());
      } else {
        REQUIRE(sel.has_value());
        REQUIRE(sublift::test_support::utf8_nfc(sel->text) ==
                sublift::test_support::utf8_nfc(
                    c.at("selected_text").get<std::string>()));
      }
    } else if (kind == "consensus") {
      std::vector<std::pair<std::string, double>> samples;
      for (const auto& s : c.at("samples")) {
        samples.emplace_back(s[0].get<std::string>(), s[1].get<double>());
      }
      const auto r =
          sublift::consensus_text(samples, c.at("script").get<std::string>());
      const auto& exp = c.at("result");
      REQUIRE(sublift::test_support::utf8_nfc(r.text) ==
              sublift::test_support::utf8_nfc(
                  exp.at("text").get<std::string>()));
      REQUIRE(r.support_votes == exp.at("support_votes").get<int>());
      REQUIRE(std::abs(r.confidence - exp.at("confidence").get<double>()) <
              1e-9);
    } else if (kind == "should_accept") {
      sublift::SubtitleProfile profile;
      profile.script = "cjk";
      const bool acc = sublift::should_accept_text(
          c.at("text").get<std::string>(), c.at("confidence").get<double>(),
          profile, 0.5, 0.28, 2);
      REQUIRE(acc == c.at("accept").get<bool>());
    }
  }
}
