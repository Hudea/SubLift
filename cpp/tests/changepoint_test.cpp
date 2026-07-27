#include <catch2/catch_test_macros.hpp>

#include "sublift/changepoint.hpp"
#include "sublift/image.hpp"
#include "sublift/signature.hpp"

#include <cstdint>
#include <optional>
#include <vector>

namespace {

sublift::FrameSignature sig(std::int64_t ts, double fg, std::uint64_t dhash = 0) {
  return sublift::FrameSignature{ts, fg, dhash};
}

std::vector<sublift::StateEvent> feed(
    sublift::ChangePointDetector& det,
    const std::vector<sublift::FrameSignature>& sigs) {
  std::vector<sublift::StateEvent> out;
  for (const auto& s : sigs) {
    if (auto e = det.process(s)) {
      out.push_back(*e);
    }
  }
  return out;
}

}  // namespace

TEST_CASE("appearance emits IN at first presence (hyst=2)", "[changepoint]") {
  sublift::ChangePointDetector det(
      sublift::ChangePointConfig{.hysteresis_frames = 2});
  const auto events = feed(det, {
                                    sig(0, 0.0),
                                    sig(200, 0.05),
                                    sig(400, 0.05),
                                });
  REQUIRE(events.size() == 1);
  REQUIRE(events[0].event_type == sublift::EventType::In);
  REQUIRE(events[0].timestamp_ms == 200);
  REQUIRE(det.current_state() == sublift::State::Stable);
}

TEST_CASE("flicker does not emit IN (hyst=2)", "[changepoint]") {
  sublift::ChangePointDetector det(
      sublift::ChangePointConfig{.hysteresis_frames = 2});
  const auto events = feed(det, {
                                    sig(0, 0.0),
                                    sig(200, 0.05),
                                    sig(400, 0.0),
                                });
  REQUIRE(events.empty());
  REQUIRE(det.current_state() == sublift::State::Empty);
}

TEST_CASE("disappearance emits OUT (hyst=2)", "[changepoint]") {
  sublift::ChangePointDetector det(
      sublift::ChangePointConfig{.hysteresis_frames = 2});
  const auto events = feed(det, {
                                    sig(0, 0.05),
                                    sig(200, 0.05),
                                    sig(400, 0.0),
                                    sig(600, 0.0),
                                });
  REQUIRE(events.size() == 2);
  REQUIRE(events[0].event_type == sublift::EventType::In);
  REQUIRE(events[1].event_type == sublift::EventType::Out);
  REQUIRE(events[1].timestamp_ms == 400);
}

TEST_CASE("dhash CHANGE with prev_end_ms", "[changepoint]") {
  sublift::ChangePointDetector det(sublift::ChangePointConfig{
      .hysteresis_frames = 2,
      .change_threshold = 5,
  });
  const auto events = feed(det, {
                                    sig(0, 0.05, 0b10101010),
                                    sig(200, 0.05, 0b10101010),
                                    sig(400, 0.05, 0b01010101),
                                    sig(600, 0.05, 0b01010101),
                                });
  std::vector<sublift::StateEvent> changes;
  for (const auto& e : events) {
    if (e.event_type == sublift::EventType::Change) {
      changes.push_back(e);
    }
  }
  REQUIRE(changes.size() == 1);
  REQUIRE(changes[0].timestamp_ms == 400);
  REQUIRE(changes[0].prev_end_ms == 400);
  REQUIRE(det.current_state() == sublift::State::StablePrime);
}

TEST_CASE("jitter no CHANGE", "[changepoint]") {
  sublift::ChangePointDetector det(sublift::ChangePointConfig{
      .hysteresis_frames = 2,
      .change_threshold = 5,
  });
  const auto events = feed(det, {
                                    sig(0, 0.05, 0b10101010),
                                    sig(200, 0.05, 0b10101010),
                                    sig(400, 0.05, 0b01010101),
                                    sig(600, 0.05, 0b10101010),
                                });
  for (const auto& e : events) {
    REQUIRE(e.event_type != sublift::EventType::Change);
  }
}

TEST_CASE("default hyst=1 single frame IN", "[changepoint]") {
  sublift::ChangePointDetector det;  // defaults
  const auto events = feed(det, {sig(200, 0.05)});
  REQUIRE(events.size() == 1);
  REQUIRE(events[0].event_type == sublift::EventType::In);
  REQUIRE(events[0].timestamp_ms == 200);
}

TEST_CASE("reset clears state", "[changepoint]") {
  sublift::ChangePointDetector det;
  (void)feed(det, {sig(0, 0.05)});
  REQUIRE(det.current_state() == sublift::State::Stable);
  det.reset();
  REQUIRE(det.current_state() == sublift::State::Empty);
}

TEST_CASE("first_presence_ms 0 is falsy like Python or", "[changepoint]") {
  // Python uses `first_presence_ms or signature.timestamp_ms`, so 0 falls through.
  sublift::ChangePointDetector det(
      sublift::ChangePointConfig{.hysteresis_frames = 2});
  const auto events = feed(det, {
                                    sig(0, 0.05),
                                    sig(200, 0.05),
                                });
  REQUIRE(events.size() == 1);
  REQUIRE(events[0].timestamp_ms == 200);
}
