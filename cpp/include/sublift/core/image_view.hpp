#pragma once

#include <cstdint>
#include <sublift/core/box.hpp>
#include <sublift/core/color.hpp>

namespace sublift {

class ImageView {
 public:
  ImageView() = default;
  ImageView(const std::uint8_t* data, std::int32_t width, std::int32_t height,
            std::int32_t stride_bytes, PixelFormat format) noexcept;

  [[nodiscard]] const std::uint8_t* data() const noexcept { return data_; }
  [[nodiscard]] std::int32_t width() const noexcept { return width_; }
  [[nodiscard]] std::int32_t height() const noexcept { return height_; }
  [[nodiscard]] std::int32_t stride_bytes() const noexcept { return stride_bytes_; }
  [[nodiscard]] PixelFormat format() const noexcept { return format_; }
  [[nodiscard]] bool empty() const noexcept {
    return data_ == nullptr || width_ <= 0 || height_ <= 0;
  }

  /// Zero-copy ROI slicing. Throws std::invalid_argument if view is empty,
  /// coordinates are negative, or bounds exceed parent view.
  [[nodiscard]] ImageView roi(std::int32_t x, std::int32_t y,
                              std::int32_t w, std::int32_t h) const;

  [[nodiscard]] ImageView roi(const Box2i& box) const {
    return roi(box.x, box.y, box.width, box.height);
  }

  [[nodiscard]] ImageView roi(const FrameLocalBox& box) const {
    return roi(box.x, box.y, box.width, box.height);
  }

  /// Direct pointer access to scanline. Throws std::invalid_argument if out of range.
  [[nodiscard]] const std::uint8_t* row(std::int32_t y) const;

 private:
  const std::uint8_t* data_{nullptr};
  std::int32_t width_{0};
  std::int32_t height_{0};
  std::int32_t stride_bytes_{0};
  PixelFormat format_{PixelFormat::RGB24};
};

} // namespace sublift
