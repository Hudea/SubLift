#pragma once

#include <cstdint>
#include <string>
#include <sublift/core/time.hpp>

namespace sublift {

struct SubtitleItem {
  std::uint64_t id{0};
  TimeRange range{};
  std::string text{};
  double confidence{1.0};

  [[nodiscard]] constexpr Timestamp start_ms() const noexcept { return range.start_ms; }
  [[nodiscard]] constexpr Timestamp end_ms() const noexcept { return range.end_ms; }
  [[nodiscard]] constexpr Timestamp duration_ms() const noexcept { return range.duration_ms(); }

  friend bool operator==(const SubtitleItem&, const SubtitleItem&) = default;
};

using SubtitleEntry = SubtitleItem;

} // namespace sublift
