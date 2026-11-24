#pragma once

#include <algorithm>
#include <concepts>
#include <sublift/core/box.hpp>

namespace sublift {

template <typename T>
concept BoxType = requires(T b) {
  { b.x } -> std::convertible_to<std::int32_t>;
  { b.y } -> std::convertible_to<std::int32_t>;
  { b.width } -> std::convertible_to<std::int32_t>;
  { b.height } -> std::convertible_to<std::int32_t>;
  { b.empty() } -> std::convertible_to<bool>;
  { b.right() } -> std::convertible_to<std::int64_t>;
  { b.bottom() } -> std::convertible_to<std::int64_t>;
};

template <BoxType BoxT>
[[nodiscard]] constexpr BoxT intersect_boxes(const BoxT& a, const BoxT& b) noexcept {
  if (a.empty() || b.empty()) {
    return BoxT{0, 0, 0, 0};
  }
  const std::int64_t x1 = std::max<std::int64_t>(a.x, b.x);
  const std::int64_t y1 = std::max<std::int64_t>(a.y, b.y);
  const std::int64_t x2 = std::min<std::int64_t>(a.right(), b.right());
  const std::int64_t y2 = std::min<std::int64_t>(a.bottom(), b.bottom());
  if (x2 <= x1 || y2 <= y1) {
    return BoxT{0, 0, 0, 0};
  }
  return BoxT{
      static_cast<std::int32_t>(x1),
      static_cast<std::int32_t>(y1),
      static_cast<std::int32_t>(x2 - x1),
      static_cast<std::int32_t>(y2 - y1),
  };
}

} // namespace sublift
