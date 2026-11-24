#pragma once

#include <optional>
#include <sublift/core/box.hpp>
#include <sublift/core/frame.hpp>

namespace sublift {

class IDetector {
 public:
  virtual ~IDetector() = default;

  [[nodiscard]] virtual std::optional<Region> detect(const Frame& frame) = 0;
};

} // namespace sublift
