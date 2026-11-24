#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#include <sublift/core/image_buffer.hpp>
#include <sublift/core/image_view.hpp>

namespace sublift::worker {

/// Downscales an RGB24 or Gray8 ImageView using bilinear interpolation if width > max_width.
/// If max_width <= 0 or src.width() <= max_width, returns a tightly packed copy.
[[nodiscard]] ImageBuffer resize_max_width(const ImageView& src, std::int32_t max_width);

/// Converts Gray8 ImageView to RGB24 ImageBuffer (or clones RGB24 tightly).
[[nodiscard]] ImageBuffer ensure_rgb24(const ImageView& src);

/// Atomically writes a P6 (RGB24) or P5 (Gray8) PPM file via temp file + rename.
/// Throws std::runtime_error on failure.
void write_ppm_atomic(const std::filesystem::path& out_path, const ImageView& image);

/// Atomically writes a minimal stored-deflate PNG (RGB24 color type 2 or Gray8 color type 0)
/// without external zlib/libpng dependencies. Throws std::runtime_error on failure.
void write_png_atomic(const std::filesystem::path& out_path, const ImageView& image);

/// Loads a binary P6 (RGB24) or P5 (Gray8) PPM file into an ImageBuffer.
/// Throws std::runtime_error on parse/IO error.
[[nodiscard]] ImageBuffer load_ppm(const std::filesystem::path& path);

}  // namespace sublift::worker
