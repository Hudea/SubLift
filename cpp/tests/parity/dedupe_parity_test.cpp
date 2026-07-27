#include <catch2/catch_test_macros.hpp>

#include "sublift/dedupe.hpp"
#include "sublift/test_support.hpp"

#include <fstream>
#include <nlohmann/json.hpp>

#ifndef SUBLIFT_PARITY_GOLDEN_DEDUPE
#error "SUBLIFT_PARITY_GOLDEN_DEDUPE required"
#endif

TEST_CASE("dedupe parity vs golden", "[parity][dedupe]") {
  std::ifstream in(SUBLIFT_PARITY_GOLDEN_DEDUPE);
  REQUIRE(in);
  nlohmann::json root;
  in >> root;
  REQUIRE(root.at("kind") == "dedupe");
  for (const auto& sc : root.at("scenarios")) {
    std::vector<sublift::SubtitleEntry> entries;
    for (const auto& e : sc.at("entries")) {
      entries.push_back(sublift::SubtitleEntry{
          e.at("start_ms").get<std::int64_t>(),
          e.at("end_ms").get<std::int64_t>(),
          e.at("text").get<std::string>(),
          e.at("confidence").get<double>(),
      });
    }
    const auto out = sublift::merge_entries(
        entries, sc.at("merge_gap_ms").get<std::int32_t>(),
        sc.at("min_duration_ms").get<std::int32_t>(),
        sc.at("drop_empty_text").get<bool>());
    const auto& exp = sc.at("result");
    REQUIRE(out.size() == exp.size());
    for (std::size_t i = 0; i < out.size(); ++i) {
      REQUIRE(out[i].start_ms == exp[i].at("start_ms").get<std::int64_t>());
      REQUIRE(out[i].end_ms == exp[i].at("end_ms").get<std::int64_t>());
      REQUIRE(sublift::test_support::utf8_nfc(out[i].text) ==
              sublift::test_support::utf8_nfc(
                  exp[i].at("text").get<std::string>()));
      REQUIRE(out[i].confidence == exp[i].at("confidence").get<double>());
    }
  }
}
