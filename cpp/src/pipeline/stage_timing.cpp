#include "sublift/pipeline/stage_timing.hpp"

#include <cstdlib>

namespace sublift {

bool stage_timing_enabled() noexcept {
  static const bool enabled = [] {
    const char* raw = std::getenv("SUBLIFT_STAGE_TIMING");
    return raw != nullptr && std::string_view(raw) == "1";
  }();
  return enabled;
}

}  // namespace sublift
