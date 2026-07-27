#include "sublift/timeline.hpp"

#include <stdexcept>

namespace sublift {

void TimelineBuilder::consume(const StateEvent& event) {
  switch (event.event_type) {
    case EventType::In:
      start_segment(event.timestamp_ms);
      break;
    case EventType::Out:
      end_segment(event.timestamp_ms);
      break;
    case EventType::Change:
      // Python: assert event.prev_end_ms is not None
      if (!event.prev_end_ms.has_value()) {
        throw std::invalid_argument(
            "TimelineBuilder: CHANGE event requires prev_end_ms");
      }
      end_segment(*event.prev_end_ms);
      start_segment(event.timestamp_ms);
      break;
  }
}

void TimelineBuilder::finalize_open_segment(std::int64_t last_timestamp_ms) {
  if (current_start_ms_.has_value()) {
    end_segment(last_timestamp_ms);
  }
}

std::vector<TimelineSegment> TimelineBuilder::build() const {
  return segments_;
}

void TimelineBuilder::reset() noexcept {
  segments_.clear();
  current_start_ms_.reset();
}

void TimelineBuilder::start_segment(std::int64_t start_ms) {
  current_start_ms_ = start_ms;
  segments_.push_back(TimelineSegment{start_ms, std::nullopt});
}

void TimelineBuilder::end_segment(std::int64_t end_ms) {
  if (!current_start_ms_.has_value()) {
    return;
  }
  segments_.back().end_ms = end_ms;
  current_start_ms_.reset();
}

}  // namespace sublift
