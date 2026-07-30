#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

namespace sublift {

/// Return the SHA-256 digest using the repository's canonical
/// "sha256:<lowercase-hex>" representation.
[[nodiscard]] std::string sha256_prefixed(
    const std::uint8_t* data,
    std::size_t size);

[[nodiscard]] inline std::string sha256_prefixed(
    std::span<const std::uint8_t> bytes) {
  return sha256_prefixed(bytes.data(), bytes.size());
}

}  // namespace sublift
