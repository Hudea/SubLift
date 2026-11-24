#pragma once

#include <cstdint>
#include <memory>
#include <sublift/core/box.hpp>
#include <sublift/core/color.hpp>
#include <sublift/core/image_view.hpp>

namespace sublift {

class ImageBuffer {
 public:
  ImageBuffer() = default;

  /// Allocate zero-filled buffer. stride_bytes == 0 means tight packing.
  ImageBuffer(std::int32_t width, std::int32_t height, PixelFormat format,
              std::int32_t stride_bytes = 0);

  /// Allocate a buffer WITHOUT zero-filling (O3).
  [[nodiscard]] static ImageBuffer allocate_uninitialized(
      std::int32_t width, std::int32_t height, PixelFormat format,
      std::int32_t stride_bytes = 0);

  ImageBuffer(ImageBuffer&& other) noexcept
      : storage_(std::move(other.storage_)),
        width_(other.width_),
        height_(other.height_),
        stride_bytes_(other.stride_bytes_),
        format_(other.format_) {
    other.width_ = 0;
    other.height_ = 0;
    other.stride_bytes_ = 0;
  }

  ImageBuffer& operator=(ImageBuffer&& other) noexcept {
    if (this != &other) {
      storage_ = std::move(other.storage_);
      width_ = other.width_;
      height_ = other.height_;
      stride_bytes_ = other.stride_bytes_;
      format_ = other.format_;
      other.width_ = 0;
      other.height_ = 0;
      other.stride_bytes_ = 0;
    }
    return *this;
  }

  ImageBuffer(const ImageBuffer&) = delete;
  ImageBuffer& operator=(const ImageBuffer&) = delete;
  ~ImageBuffer() = default;

  [[nodiscard]] std::int32_t width() const noexcept { return width_; }
  [[nodiscard]] std::int32_t height() const noexcept { return height_; }
  [[nodiscard]] std::int32_t stride_bytes() const noexcept { return stride_bytes_; }
  [[nodiscard]] PixelFormat format() const noexcept { return format_; }
  [[nodiscard]] bool empty() const noexcept {
    return width_ <= 0 || height_ <= 0 || storage_ == nullptr;
  }

  [[nodiscard]] std::uint8_t* data() noexcept { return storage_.get(); }
  [[nodiscard]] const std::uint8_t* data() const noexcept { return storage_.get(); }

  [[nodiscard]] std::uint8_t* row(std::int32_t y) noexcept {
    if (empty() || y < 0 || y >= height_) {
      return nullptr;
    }
    return storage_.get() + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride_bytes_);
  }

  [[nodiscard]] const std::uint8_t* row(std::int32_t y) const noexcept {
    if (empty() || y < 0 || y >= height_) {
      return nullptr;
    }
    return storage_.get() + static_cast<std::size_t>(y) * static_cast<std::size_t>(stride_bytes_);
  }

  [[nodiscard]] ImageView view() const noexcept;
  [[nodiscard]] ImageView roi(std::int32_t x, std::int32_t y, std::int32_t w,
                              std::int32_t h) const;
  [[nodiscard]] ImageView roi(const Box2i& box) const;
  [[nodiscard]] ImageView roi(const FrameLocalBox& box) const;

  /// Deep copy of the full buffer.
  [[nodiscard]] ImageBuffer clone() const;

 private:
  static ImageBuffer allocate(std::int32_t width, std::int32_t height,
                              PixelFormat format, std::int32_t stride_bytes,
                              bool zero_fill);

  std::unique_ptr<std::uint8_t[]> storage_{};
  std::int32_t width_{0};
  std::int32_t height_{0};
  std::int32_t stride_bytes_{0};
  PixelFormat format_{PixelFormat::RGB24};
};

} // namespace sublift
