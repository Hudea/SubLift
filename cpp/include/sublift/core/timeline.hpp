#pragma once

#include <cstdint>
#include <optional>
#include <vector>

#include "sublift/changepoint.hpp"

namespace sublift {

/// Matches Python `TimelineSegment`.
struct TimelineSegment {
  std::int64_t start_ms{0};
  std::optional<std::int64_t> end_ms{};

  [[nodiscard]] std::int64_t duration_ms() const noexcept {
    if (!end_ms.has_value()) {
      return 0;
    }
    return *end_ms - start_ms;
  }

  friend constexpr bool operator==(const TimelineSegment&,
                                   const TimelineSegment&) = default;
};

/// Matches Python `TimelineBuilder`.
class TimelineBuilder {
 public:
  void consume(const StateEvent& event);
  void finalize_open_segment(std::int64_t last_timestamp_ms);
  [[nodiscard]] std::vector<TimelineSegment> build() const;
  void reset() noexcept;

 private:
  void start_segment(std::int64_t start_ms);
  void end_segment(std::int64_t end_ms);

  std::vector<TimelineSegment> segments_{};
  std::optional<std::int64_t> current_start_ms_{};
};

}  // namespace sublift
