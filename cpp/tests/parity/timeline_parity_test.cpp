#include <catch2/catch_test_macros.hpp>

#include "sublift/timeline.hpp"

#include <fstream>
#include <nlohmann/json.hpp>

#ifndef SUBLIFT_PARITY_GOLDEN_TIMELINE
#error "SUBLIFT_PARITY_GOLDEN_TIMELINE required"
#endif

TEST_CASE("timeline parity vs golden", "[parity][timeline]") {
  std::ifstream in(SUBLIFT_PARITY_GOLDEN_TIMELINE);
  REQUIRE(in);
  nlohmann::json root;
  in >> root;
  REQUIRE(root.at("kind") == "timeline");
  for (const auto& sc : root.at("scenarios")) {
    sublift::TimelineBuilder b;
    for (const auto& e : sc.at("events")) {
      sublift::EventType et = sublift::EventType::In;
      const auto name = e.at("event_type").get<std::string>();
      if (name == "OUT") {
        et = sublift::EventType::Out;
      } else if (name == "CHANGE") {
        et = sublift::EventType::Change;
      }
      std::optional<std::int64_t> prev;
      if (!e.at("prev_end_ms").is_null()) {
        prev = e.at("prev_end_ms").get<std::int64_t>();
      }
      b.consume(sublift::StateEvent{et, e.at("timestamp_ms").get<std::int64_t>(), prev});
    }
    if (!sc.at("finalize_ms").is_null()) {
      b.finalize_open_segment(sc.at("finalize_ms").get<std::int64_t>());
    }
    const auto segs = b.build();
    const auto& exp = sc.at("segments");
    REQUIRE(segs.size() == exp.size());
    for (std::size_t i = 0; i < segs.size(); ++i) {
      REQUIRE(segs[i].start_ms == exp[i].at("start_ms").get<std::int64_t>());
      if (exp[i].at("end_ms").is_null()) {
        REQUIRE_FALSE(segs[i].end_ms.has_value());
      } else {
        REQUIRE(segs[i].end_ms == exp[i].at("end_ms").get<std::int64_t>());
      }
    }
  }
}
