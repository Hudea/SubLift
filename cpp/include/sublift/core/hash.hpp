#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
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

/// 64-character lowercase hex digest without a prefix.
[[nodiscard]] std::string sha256_hex(const std::uint8_t* data, std::size_t size);

[[nodiscard]] inline std::string sha256_hex(std::span<const std::uint8_t> bytes) {
  return sha256_hex(bytes.data(), bytes.size());
}

/// Hash a regular file. Returns nullopt if the file cannot be read.
[[nodiscard]] std::optional<std::string> sha256_file_hex(
    const std::filesystem::path& path);

}  // namespace sublift
