#include <catch2/catch_test_macros.hpp>

#include "sublift/timeline.hpp"

#include <stdexcept>

TEST_CASE("timeline single segment", "[timeline]") {
  sublift::TimelineBuilder b;
  b.consume(sublift::StateEvent{sublift::EventType::In, 0, std::nullopt});
  b.consume(sublift::StateEvent{sublift::EventType::Out, 1000, std::nullopt});
  const auto segs = b.build();
  REQUIRE(segs.size() == 1);
  REQUIRE(segs[0].start_ms == 0);
  REQUIRE(segs[0].end_ms == 1000);
}

TEST_CASE("timeline CHANGE splits", "[timeline]") {
  sublift::TimelineBuilder b;
  b.consume(sublift::StateEvent{sublift::EventType::In, 0, std::nullopt});
  b.consume(sublift::StateEvent{sublift::EventType::Change, 500, 500});
  b.consume(sublift::StateEvent{sublift::EventType::Out, 1000, std::nullopt});
  const auto segs = b.build();
  REQUIRE(segs.size() == 2);
  REQUIRE(segs[0].end_ms == 500);
  REQUIRE(segs[1].start_ms == 500);
}

TEST_CASE("timeline finalize open", "[timeline]") {
  sublift::TimelineBuilder b;
  b.consume(sublift::StateEvent{sublift::EventType::In, 0, std::nullopt});
  b.finalize_open_segment(2000);
  REQUIRE(b.build()[0].end_ms == 2000);
}

TEST_CASE("timeline reset", "[timeline]") {
  sublift::TimelineBuilder b;
  b.consume(sublift::StateEvent{sublift::EventType::In, 0, std::nullopt});
  b.reset();
  REQUIRE(b.build().empty());
}

TEST_CASE("timeline CHANGE without prev_end_ms throws", "[timeline]") {
  sublift::TimelineBuilder b;
  b.consume(sublift::StateEvent{sublift::EventType::In, 0, std::nullopt});
  REQUIRE_THROWS_AS(
      b.consume(sublift::StateEvent{sublift::EventType::Change, 500, std::nullopt}),
      std::invalid_argument);
}
