#pragma once

#include <cstdint>

namespace sublift::features {

struct FrameSignature {
  std::int64_t timestamp_ms{0};
  double foreground_ratio{0.0};
  std::uint64_t dhash{0};

  friend bool operator==(const FrameSignature&, const FrameSignature&) = default;
};

} // namespace sublift::features
