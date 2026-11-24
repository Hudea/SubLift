#include <cstring>
#include <limits>
#include <memory>
#include <stdexcept>
#include <sublift/core/image_buffer.hpp>

namespace sublift {

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
    return ImageView{};
  }
  return ImageView{data(), width_, height_, stride_bytes_, format_};
}

ImageView ImageBuffer::roi(std::int32_t x, std::int32_t y, std::int32_t w,
                           std::int32_t h) const {
  return view().roi(x, y, w, h);
}

ImageView ImageBuffer::roi(const Box2i& box) const {
  return view().roi(box.x, box.y, box.width, box.height);
}

ImageView ImageBuffer::roi(const FrameLocalBox& box) const {
  return view().roi(box.x, box.y, box.width, box.height);
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

} // namespace sublift
