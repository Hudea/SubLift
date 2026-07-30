#pragma once

#include <string_view>

namespace sublift {

/// Library version string for smoke / CLI banners (stub until packaging).
[[nodiscard]] std::string_view version() noexcept;

}  // namespace sublift
