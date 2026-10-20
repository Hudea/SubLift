#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

namespace sublift {

/// Stage timing accumulator (Phase 16 D01 / S0).
///
/// Pure measurement infrastructure for the C++ Native attribution baseline.
/// Disabled by default and enabled only via `SUBLIFT_STAGE_TIMING=1`; when
/// disabled every helper is a no-op with a single static-flag check, so the
/// product path carries no measurable overhead and zero behavior change.
///
/// Usage (measurement code only, never product logic):
///   {
///     StageTimer timer(registry, "signature");
///     ... measured region ...
///   }
///
/// The registry is a plain accumulator: callers must serialize access (the
/// Pipeline is single-consumer by contract; the bridge extract loop and OCR
/// calls are serial per job). `PaddleStageTiming` provides a mutex-guarded
/// process-level registry because the Paddle adapter sits behind the generic
/// `IOcrEngine` port.
struct StageTimingEntry {
  std::uint64_t calls{0};
  double total_ms{0.0};
};

/// Serial-use accumulator keyed by stage name.
class StageTimingRegistry {
 public:
  void add(std::string_view stage, double ms) {
    auto& entry = stages_[std::string(stage)];
    entry.calls += 1;
    entry.total_ms += ms;
  }

  [[nodiscard]] const std::map<std::string, StageTimingEntry>& stages() const noexcept {
    return stages_;
  }

  [[nodiscard]] bool empty() const noexcept { return stages_.empty(); }

  void clear() noexcept { stages_.clear(); }

 private:
  std::map<std::string, StageTimingEntry> stages_;
};

/// Mutex-guarded process-level registry for adapter-internal stages
/// (Paddle det/cls/rec + preprocess), reachable behind the IOcrEngine port.
class SharedStageTimingRegistry {
 public:
  void add(std::string_view stage, double ms) {
    std::lock_guard<std::mutex> lock(mutex_);
    registry_.add(stage, ms);
  }

  [[nodiscard]] std::map<std::string, StageTimingEntry> snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return registry_.stages();
  }

  void clear() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    registry_.clear();
  }

 private:
  mutable std::mutex mutex_;
  StageTimingRegistry registry_;
};

/// Whether stage timing is enabled for this process (`SUBLIFT_STAGE_TIMING=1`).
/// Cached after the first call; the environment is not expected to change
/// mid-process (documented as a process-level measurement switch).
[[nodiscard]] bool stage_timing_enabled() noexcept;

/// RAII scope timer. No-op when stage timing is disabled.
class StageTimer {
 public:
  StageTimer(StageTimingRegistry& registry, std::string_view stage)
      : registry_(&registry),
        stage_(stage),
        enabled_(stage_timing_enabled()),
        start_(std::chrono::steady_clock::now()) {}

  StageTimer(SharedStageTimingRegistry& registry, std::string_view stage)
      : shared_registry_(&registry),
        stage_(stage),
        enabled_(stage_timing_enabled()),
        start_(std::chrono::steady_clock::now()) {}

  ~StageTimer() {
    if (!enabled_) {
      return;
    }
    const auto end = std::chrono::steady_clock::now();
    const double ms =
        std::chrono::duration<double, std::milli>(end - start_).count();
    if (shared_registry_ != nullptr) {
      shared_registry_->add(stage_, ms);
    } else {
      registry_->add(stage_, ms);
    }
  }

  StageTimer(const StageTimer&) = delete;
  StageTimer& operator=(const StageTimer&) = delete;
  StageTimer(StageTimer&&) = delete;
  StageTimer& operator=(StageTimer&&) = delete;

 private:
  StageTimingRegistry* registry_{nullptr};
  SharedStageTimingRegistry* shared_registry_{nullptr};
  std::string stage_;
  bool enabled_{false};
  std::chrono::steady_clock::time_point start_;
};

}  // namespace sublift
