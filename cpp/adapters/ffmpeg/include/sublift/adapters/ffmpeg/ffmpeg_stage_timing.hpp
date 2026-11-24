#pragma once

#include <sublift/core/stage_timing.hpp>

namespace sublift::adapters::ffmpeg {

[[nodiscard]] inline SharedStageTimingRegistry& stage_registry() noexcept {
  static SharedStageTimingRegistry registry;
  return registry;
}

}  // namespace sublift::adapters::ffmpeg
