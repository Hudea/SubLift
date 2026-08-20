#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace sublift::cli {

/// Product hosts ignore `SUBLIFT_RUNTIME` except when it requests a non-Native
/// runtime. Unset and `cpp` continue; `python` or any other value is an error.
[[nodiscard]] std::optional<std::string> product_runtime_env_error(
    std::string_view env_value);

[[nodiscard]] std::optional<std::string> product_runtime_env_error_from_process();

}  // namespace sublift::cli
