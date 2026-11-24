#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <sublift/core/image_view.hpp>

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

} // namespace

ImageView::ImageView(const std::uint8_t* data, std::int32_t width, std::int32_t height,
                     std::int32_t stride_bytes, PixelFormat format) noexcept
    : data_(data),
      width_(width),
      height_(height),
      stride_bytes_(stride_bytes > 0 ? stride_bytes : width * bytes_per_pixel(format)),
      format_(format) {
  if (width_ <= 0 || height_ <= 0 || data_ == nullptr) {
    data_ = nullptr;
    width_ = 0;
    height_ = 0;
    stride_bytes_ = 0;
  }
}

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
  const auto* ptr = data_ +
                    static_cast<std::size_t>(y) * static_cast<std::size_t>(stride_bytes_) +
                    static_cast<std::size_t>(x) * static_cast<std::size_t>(bpp);
  return ImageView{ptr, w, h, stride_bytes_, format_};
}

const std::uint8_t* ImageView::row(std::int32_t y) const {
  if (empty() || y < 0 || y >= height_) {
    throw std::invalid_argument("ImageView::row: out of bounds");
  }
  return data_ + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride_bytes_);
}

} // namespace sublift
