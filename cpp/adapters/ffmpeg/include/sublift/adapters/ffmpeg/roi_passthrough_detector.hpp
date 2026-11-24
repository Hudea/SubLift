#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

#include <sublift/core/box.hpp>
#include <sublift/core/frame.hpp>
#include <sublift/ports/detector.hpp>

namespace sublift::adapters::ffmpeg {

class RoiPassthroughDetector final : public IDetector {
 public:
  RoiPassthroughDetector(std::int32_t width, std::int32_t height)
      : box_{0, 0, width, height} {
    if (width <= 0 || height <= 0) {
      throw std::invalid_argument(
          "RoiPassthroughDetector 需要正尺寸，收到 width=" +
          std::to_string(width) + " height=" + std::to_string(height));
    }
  }

  [[nodiscard]] FrameLocalBox box() const noexcept { return box_; }

  [[nodiscard]] std::optional<Region> detect(const Frame& /*frame*/) override {
    return Region{box_};
  }

 private:
  FrameLocalBox box_{};
};

}  // namespace sublift::adapters::ffmpeg
