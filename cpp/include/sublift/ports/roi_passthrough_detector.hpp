#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>

#include "sublift/detector.hpp"
#include "sublift/models.hpp"

namespace sublift {

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

  [[nodiscard]] bool is_equal(const IDetector& other) const noexcept override {
    if (auto* p = dynamic_cast<const RoiPassthroughDetector*>(&other)) {
      return box_ == p->box_;
    }
    return false;
  }

 private:
  FrameLocalBox box_{};
};

}  // namespace sublift
