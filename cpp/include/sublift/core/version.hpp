#pragma once

#include <string_view>

#define SUBLIFT_VERSION_MAJOR 1
#define SUBLIFT_VERSION_MINOR 0
#define SUBLIFT_VERSION_PATCH 0

namespace sublift {

[[nodiscard]] constexpr std::string_view version_string() noexcept {
  return "1.0.0";
}

} // namespace sublift
