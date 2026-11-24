#pragma once

#include <algorithm>
#include <cstdint>

namespace sublift {

struct Box2i {
  std::int32_t x{0};
  std::int32_t y{0};
  std::int32_t width{0};
  std::int32_t height{0};

  [[nodiscard]] constexpr bool empty() const noexcept {
    return width <= 0 || height <= 0;
  }

  [[nodiscard]] constexpr std::int64_t right() const noexcept {
    return static_cast<std::int64_t>(x) + static_cast<std::int64_t>(width);
  }

  [[nodiscard]] constexpr std::int64_t bottom() const noexcept {
    return static_cast<std::int64_t>(y) + static_cast<std::int64_t>(height);
  }

  [[nodiscard]] constexpr bool contains(std::int32_t px, std::int32_t py) const noexcept {
    if (empty()) {
      return false;
    }
    return static_cast<std::int64_t>(px) >= static_cast<std::int64_t>(x) &&
           static_cast<std::int64_t>(px) < right() &&
           static_cast<std::int64_t>(py) >= static_cast<std::int64_t>(y) &&
           static_cast<std::int64_t>(py) < bottom();
  }

  [[nodiscard]] constexpr bool contains(const Box2i& other) const noexcept {
    if (empty() || other.empty()) {
      return false;
    }
    return static_cast<std::int64_t>(other.x) >= static_cast<std::int64_t>(x) &&
           other.right() <= right() &&
           static_cast<std::int64_t>(other.y) >= static_cast<std::int64_t>(y) &&
           other.bottom() <= bottom();
  }

  [[nodiscard]] constexpr bool intersects(const Box2i& other) const noexcept {
    if (empty() || other.empty()) {
      return false;
    }
    return static_cast<std::int64_t>(x) < other.right() &&
           right() > static_cast<std::int64_t>(other.x) &&
           static_cast<std::int64_t>(y) < other.bottom() &&
           bottom() > static_cast<std::int64_t>(other.y);
  }

  [[nodiscard]] constexpr Box2i intersection(const Box2i& other) const noexcept {
    if (empty() || other.empty()) {
      return Box2i{0, 0, 0, 0};
    }
    const std::int64_t x1 = std::max<std::int64_t>(x, other.x);
    const std::int64_t y1 = std::max<std::int64_t>(y, other.y);
    const std::int64_t x2 = std::min<std::int64_t>(right(), other.right());
    const std::int64_t y2 = std::min<std::int64_t>(bottom(), other.bottom());
    if (x2 <= x1 || y2 <= y1) {
      return Box2i{0, 0, 0, 0};
    }
    return Box2i{
        static_cast<std::int32_t>(x1),
        static_cast<std::int32_t>(y1),
        static_cast<std::int32_t>(x2 - x1),
        static_cast<std::int32_t>(y2 - y1),
    };
  }

  friend constexpr bool operator==(const Box2i&, const Box2i&) = default;
};

// 视频全画幅坐标系（源视频全帧尺寸）
struct SourceBox {
  std::int32_t x{0};
  std::int32_t y{0};
  std::int32_t width{0};
  std::int32_t height{0};

  [[nodiscard]] constexpr bool empty() const noexcept {
    return width <= 0 || height <= 0;
  }
  [[nodiscard]] constexpr std::int64_t right() const noexcept {
    return static_cast<std::int64_t>(x) + static_cast<std::int64_t>(width);
  }
  [[nodiscard]] constexpr std::int64_t bottom() const noexcept {
    return static_cast<std::int64_t>(y) + static_cast<std::int64_t>(height);
  }
  friend constexpr bool operator==(const SourceBox&, const SourceBox&) = default;
};

// 抽帧产物局部坐标系（经过粗剪/ROI 后的图像帧内）
struct FrameLocalBox {
  std::int32_t x{0};
  std::int32_t y{0};
  std::int32_t width{0};
  std::int32_t height{0};

  [[nodiscard]] constexpr bool empty() const noexcept {
    return width <= 0 || height <= 0;
  }
  [[nodiscard]] constexpr std::int64_t right() const noexcept {
    return static_cast<std::int64_t>(x) + static_cast<std::int64_t>(width);
  }
  [[nodiscard]] constexpr std::int64_t bottom() const noexcept {
    return static_cast<std::int64_t>(y) + static_cast<std::int64_t>(height);
  }
  friend constexpr bool operator==(const FrameLocalBox&, const FrameLocalBox&) = default;
};

// OCR 识别局部坐标系（送入 OCR 引擎的裁剪块坐标）
struct OcrCropBox {
  std::int32_t x{0};
  std::int32_t y{0};
  std::int32_t width{0};
  std::int32_t height{0};

  [[nodiscard]] constexpr bool empty() const noexcept {
    return width <= 0 || height <= 0;
  }
  [[nodiscard]] constexpr std::int64_t right() const noexcept {
    return static_cast<std::int64_t>(x) + static_cast<std::int64_t>(width);
  }
  [[nodiscard]] constexpr std::int64_t bottom() const noexcept {
    return static_cast<std::int64_t>(y) + static_cast<std::int64_t>(height);
  }
  friend constexpr bool operator==(const OcrCropBox&, const OcrCropBox&) = default;
};

struct Region {
  FrameLocalBox box{};
  [[nodiscard]] constexpr bool empty() const noexcept { return box.empty(); }
  friend constexpr bool operator==(const Region&, const Region&) = default;
};

} // namespace sublift
