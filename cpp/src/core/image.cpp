#include "sublift/image.hpp"

#include <cstring>
#include <limits>

namespace sublift {
namespace {

void validate_roi(std::int32_t x, std::int32_t y, std::int32_t w, std::int32_t h,
                  std::int32_t width, std::int32_t height) {
  if (x < 0 || y < 0 || w < 0 || h < 0) {
    throw std::invalid_argument("ImageView::roi: negative coordinates or size");
  }
  if (static_cast<std::int64_t>(x) + w > width ||
      static_cast<std::int64_t>(y) + h > height) {
    throw std::invalid_argument("ImageView::roi: out of bounds");
  }
}

}  // namespace

ImageView ImageView::roi(std::int32_t x, std::int32_t y, std::int32_t w,
                         std::int32_t h) const {
  if (empty()) {
    throw std::invalid_argument("ImageView::roi: empty view");
  }
  validate_roi(x, y, w, h, width_, height_);
  if (w == 0 || h == 0) {
    return ImageView{nullptr, 0, 0, stride_bytes_, format_};
  }
  const int bpp = bytes_per_pixel(format_);
  const auto* ptr = data_ + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride_bytes_) +
                    static_cast<std::size_t>(x) * static_cast<std::size_t>(bpp);
  return ImageView{ptr, w, h, stride_bytes_, format_};
}

const std::uint8_t* ImageView::row(std::int32_t y) const {
  if (empty() || y < 0 || y >= height_) {
    throw std::invalid_argument("ImageView::row: out of bounds");
  }
  return data_ + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride_bytes_);
}

ImageBuffer ImageBuffer::allocate(std::int32_t width, std::int32_t height,
                                  PixelFormat format, std::int32_t stride_bytes,
                                  bool zero_fill) {
  ImageBuffer buf;
  buf.width_ = width;
  buf.height_ = height;
  buf.format_ = format;
  if (width < 0 || height < 0) {
    throw std::invalid_argument("ImageBuffer: negative dimensions");
  }
  if (width == 0 || height == 0) {
    buf.width_ = 0;
    buf.height_ = 0;
    buf.stride_bytes_ = 0;
    return buf;
  }
  const int bpp = bytes_per_pixel(format);
  if (bpp <= 0) {
    throw std::invalid_argument("ImageBuffer: unknown pixel format");
  }

  // Promote to int64 so width * bpp cannot wrap around int32 and bypass checks.
  const std::int64_t min_stride64 =
      static_cast<std::int64_t>(width) * static_cast<std::int64_t>(bpp);
  if (min_stride64 > std::numeric_limits<std::int32_t>::max()) {
    throw std::invalid_argument("ImageBuffer: width*bpp exceeds int32 stride");
  }
  const std::int32_t min_stride = static_cast<std::int32_t>(min_stride64);

  if (stride_bytes == 0) {
    buf.stride_bytes_ = min_stride;
  } else {
    if (stride_bytes < min_stride) {
      throw std::invalid_argument("ImageBuffer: stride too small");
    }
    buf.stride_bytes_ = stride_bytes;
  }

  const std::uint64_t nbytes64 =
      static_cast<std::uint64_t>(static_cast<std::uint32_t>(buf.stride_bytes_)) *
      static_cast<std::uint64_t>(static_cast<std::uint32_t>(buf.height_));
  if (nbytes64 > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max())) {
    throw std::invalid_argument("ImageBuffer: allocation size too large");
  }
  const std::size_t nbytes = static_cast<std::size_t>(nbytes64);
  // Default-initialize (no zero-fill); the zero-filled path memsets below so
  // the public constructor keeps its exact historical semantics.
  buf.storage_ = std::make_unique_for_overwrite<std::uint8_t[]>(nbytes);
  if (zero_fill) {
    std::memset(buf.storage_.get(), 0, nbytes);
  }
  return buf;
}

ImageBuffer::ImageBuffer(std::int32_t width, std::int32_t height, PixelFormat format,
                         std::int32_t stride_bytes)
    : ImageBuffer(allocate(width, height, format, stride_bytes, /*zero_fill=*/true)) {}

ImageBuffer ImageBuffer::allocate_uninitialized(std::int32_t width,
                                                std::int32_t height,
                                                PixelFormat format,
                                                std::int32_t stride_bytes) {
  return allocate(width, height, format, stride_bytes, /*zero_fill=*/false);
}

ImageView ImageBuffer::view() const noexcept {
  if (empty()) {
    return {};
  }
  return ImageView{data(), width_, height_, stride_bytes_, format_};
}

ImageView ImageBuffer::roi(std::int32_t x, std::int32_t y, std::int32_t w,
                           std::int32_t h) const {
  return view().roi(x, y, w, h);
}

ImageBuffer ImageBuffer::clone() const {
  if (empty()) {
    return ImageBuffer{};
  }
  // The clone overwrites the whole buffer, so skip the zero-fill (O3).
  ImageBuffer out = allocate_uninitialized(width_, height_, format_, stride_bytes_);
  std::memcpy(out.storage_.get(), storage_.get(),
              static_cast<std::size_t>(stride_bytes_) *
                  static_cast<std::size_t>(height_));
  return out;
}

}  // namespace sublift
