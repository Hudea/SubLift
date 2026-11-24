#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>

namespace sublift::adapters::paddle::detail {

[[nodiscard]] std::string sha256_prefixed(
    const std::uint8_t* data,
    std::size_t size);

[[nodiscard]] inline std::string sha256_prefixed(
    std::span<const std::uint8_t> bytes) {
  return sha256_prefixed(bytes.data(), bytes.size());
}

[[nodiscard]] std::string sha256_hex(const std::uint8_t* data, std::size_t size);

[[nodiscard]] inline std::string sha256_hex(std::span<const std::uint8_t> bytes) {
  return sha256_hex(bytes.data(), bytes.size());
}

[[nodiscard]] std::optional<std::string> sha256_file_hex(
    const std::filesystem::path& path);

}  // namespace sublift::adapters::paddle::detail
