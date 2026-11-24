#pragma once

#include <cstdint>
#include <sublift/core/image_buffer.hpp>

namespace sublift {

struct Frame {
  std::int64_t timestamp_ms{0};
  ImageBuffer image{};
};

} // namespace sublift
