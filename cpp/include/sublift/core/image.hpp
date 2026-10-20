#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <utility>

namespace sublift {

enum class PixelFormat : std::uint8_t {
  RGB24 = 0,
  BGR24 = 1,
  Gray8 = 2,
};

[[nodiscard]] constexpr int bytes_per_pixel(PixelFormat format) noexcept {
  switch (format) {
    case PixelFormat::RGB24:
    case PixelFormat::BGR24:
      return 3;
    case PixelFormat::Gray8:
      return 1;
  }
  return 0;
}

/// Non-owning, read-only view into pixel memory.
class ImageView {
 public:
  ImageView() = default;

  ImageView(const std::uint8_t* data, std::int32_t width, std::int32_t height,
            std::int32_t stride_bytes, PixelFormat format) noexcept
      : data_(data),
        width_(width),
        height_(height),
        stride_bytes_(stride_bytes),
        format_(format) {}

  [[nodiscard]] const std::uint8_t* data() const noexcept { return data_; }
  [[nodiscard]] std::int32_t width() const noexcept { return width_; }
  [[nodiscard]] std::int32_t height() const noexcept { return height_; }
  [[nodiscard]] std::int32_t stride_bytes() const noexcept { return stride_bytes_; }
  [[nodiscard]] PixelFormat format() const noexcept { return format_; }
  [[nodiscard]] bool empty() const noexcept {
    return data_ == nullptr || width_ <= 0 || height_ <= 0;
  }

  /// Shared ROI (same underlying storage). Throws std::invalid_argument on OOB.
  [[nodiscard]] ImageView roi(std::int32_t x, std::int32_t y, std::int32_t w,
                              std::int32_t h) const;

  [[nodiscard]] const std::uint8_t* row(std::int32_t y) const;

 private:
  const std::uint8_t* data_{nullptr};
  std::int32_t width_{0};
  std::int32_t height_{0};
  std::int32_t stride_bytes_{0};
  PixelFormat format_{PixelFormat::RGB24};
};

/// Owning image buffer. Move-only; deep copy via clone().
class ImageBuffer {
 public:
  ImageBuffer() = default;

  /// Allocate zero-filled buffer. stride_bytes==0 means tight packing.
  ImageBuffer(std::int32_t width, std::int32_t height, PixelFormat format,
              std::int32_t stride_bytes = 0);

  /// Allocate a buffer WITHOUT zero-filling (O3). INTERNAL hot-path factory:
  /// the caller must fully overwrite the storage before any read of it, and
  /// must never let uninitialized bytes reach an output or error path. Not
  /// part of the Ports API; used only where the allocation is immediately
  /// followed by a complete overwrite (frame extraction, RGB->BGR
  /// conversion, clone).
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

  [[nodiscard]] ImageView view() const noexcept;
  [[nodiscard]] ImageView roi(std::int32_t x, std::int32_t y, std::int32_t w,
                              std::int32_t h) const;

  /// Deep copy of the full buffer.
  [[nodiscard]] ImageBuffer clone() const;

 private:
  /// Shared allocation/validation core. `zero_fill=false` leaves the storage
  /// uninitialized (caller must overwrite before reading).
  static ImageBuffer allocate(std::int32_t width, std::int32_t height,
                              PixelFormat format, std::int32_t stride_bytes,
                              bool zero_fill);

  std::unique_ptr<std::uint8_t[]> storage_{};
  std::int32_t width_{0};
  std::int32_t height_{0};
  std::int32_t stride_bytes_{0};
  PixelFormat format_{PixelFormat::RGB24};
};

}  // namespace sublift
