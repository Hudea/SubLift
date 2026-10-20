#pragma once

#include "sublift/pipeline/stage_timing.hpp"

namespace sublift::ffmpeg {

/// Process-level stage timing registry for extractor internals (the
/// "extract_wait" stage: process spawn + pipe wait + per-frame buffer
/// materialization + teardown). Measurement only (Phase 16 D01 / S0):
/// inert unless `SUBLIFT_STAGE_TIMING=1`.
///
/// Declared separately from `sublift/adapters/ffmpeg.hpp` so measurement
/// consumers (the worker bridge) do not need the adapter's JSON/port
/// dependencies.
[[nodiscard]] SharedStageTimingRegistry& stage_registry() noexcept;

}  // namespace sublift::ffmpeg
