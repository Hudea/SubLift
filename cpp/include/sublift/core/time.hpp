#pragma once

#include <cstdint>

namespace sublift {

using Timestamp = std::int64_t; // 毫秒

struct TimeRange {
  Timestamp start_ms{0};
  Timestamp end_ms{0};

  [[nodiscard]] constexpr Timestamp duration_ms() const noexcept {
    return end_ms - start_ms;
  }

  [[nodiscard]] constexpr bool empty() const noexcept {
    return duration_ms() <= 0;
  }

  [[nodiscard]] constexpr bool contains(Timestamp ms) const noexcept {
    if (empty()) {
      return false;
    }
    return ms >= start_ms && ms < end_ms;
  }

  [[nodiscard]] constexpr bool contains(const TimeRange& other) const noexcept {
    if (empty() || other.empty()) {
      return false;
    }
    return other.start_ms >= start_ms && other.end_ms <= end_ms;
  }

  [[nodiscard]] constexpr bool overlaps(const TimeRange& other) const noexcept {
    if (empty() || other.empty()) {
      return false;
    }
    return start_ms < other.end_ms && end_ms > other.start_ms;
  }

  friend constexpr bool operator==(const TimeRange&, const TimeRange&) = default;
};

} // namespace sublift
