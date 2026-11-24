#pragma once

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>
#include <string_view>

namespace sublift {

struct StageTimingEntry {
  std::uint64_t calls{0};
  double total_ms{0.0};
};

class StageTimingRegistry {
 public:
  void add(std::string_view stage, double ms) {
    auto& entry = stages_[std::string(stage)];
    entry.calls += 1;
    entry.total_ms += ms;
  }

  void merge_from(const StageTimingRegistry& other) {
    for (const auto& [stage, entry] : other.stages_) {
      auto& dst = stages_[stage];
      dst.calls += entry.calls;
      dst.total_ms += entry.total_ms;
    }
  }

  [[nodiscard]] const std::map<std::string, StageTimingEntry>& stages() const noexcept {
    return stages_;
  }

  [[nodiscard]] bool empty() const noexcept { return stages_.empty(); }

  void clear() noexcept { stages_.clear(); }

 private:
  std::map<std::string, StageTimingEntry> stages_;
};

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

[[nodiscard]] inline bool stage_timing_enabled() noexcept {
  static const bool enabled = []() {
    const char* env = std::getenv("SUBLIFT_STAGE_TIMING");
    return env != nullptr && std::string_view(env) == "1";
  }();
  return enabled;
}

class StageTimer {
 public:
  StageTimer(StageTimingRegistry& registry, std::string_view stage)
      : registry_(&registry),
        enabled_(stage_timing_enabled()),
        stage_(enabled_ ? stage : std::string_view{}),
        start_(enabled_ ? std::chrono::steady_clock::now()
                        : std::chrono::steady_clock::time_point{}) {}

  StageTimer(StageTimingRegistry* registry, std::string_view stage)
      : registry_(registry),
        enabled_(registry != nullptr && stage_timing_enabled()),
        stage_(enabled_ ? stage : std::string_view{}),
        start_(enabled_ ? std::chrono::steady_clock::now()
                        : std::chrono::steady_clock::time_point{}) {}

  StageTimer(SharedStageTimingRegistry& registry, std::string_view stage)
      : shared_registry_(&registry),
        enabled_(stage_timing_enabled()),
        stage_(enabled_ ? stage : std::string_view{}),
        start_(enabled_ ? std::chrono::steady_clock::now()
                        : std::chrono::steady_clock::time_point{}) {}

  ~StageTimer() {
    if (!enabled_) {
      return;
    }
    const auto end = std::chrono::steady_clock::now();
    const double ms =
        std::chrono::duration<double, std::milli>(end - start_).count();
    if (shared_registry_ != nullptr) {
      shared_registry_->add(stage_, ms);
    } else if (registry_ != nullptr) {
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
  bool enabled_{false};
  std::string stage_;
  std::chrono::steady_clock::time_point start_;
};

} // namespace sublift
