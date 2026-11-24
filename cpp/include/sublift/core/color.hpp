#pragma once

#include <cstdint>

namespace sublift {

enum class PixelFormat : std::uint8_t {
  RGB24 = 0,
  Gray8 = 1,
};

[[nodiscard]] constexpr int bytes_per_pixel(PixelFormat format) noexcept {
  return (format == PixelFormat::RGB24) ? 3 : 1;
}

} // namespace sublift
