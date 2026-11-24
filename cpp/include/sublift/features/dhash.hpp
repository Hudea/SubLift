#pragma once

#include <cstdint>
#include <sublift/core/image_view.hpp>

namespace sublift::features {

[[nodiscard]] std::uint64_t compute_dhash(const ImageView& image, int hash_size = 8);
[[nodiscard]] std::uint64_t compute_sector_dhash(const ImageView& image);
[[nodiscard]] int hamming_distance(std::uint64_t h1, std::uint64_t h2) noexcept;

} // namespace sublift::features
