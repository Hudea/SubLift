#pragma once

#include <optional>

#include "sublift/models.hpp"

namespace sublift {

/// Subtitle region detector abstraction. Matches the Python `Detector` Protocol
/// in `src/sublift/detector/base.py`.
///
/// The Python pipeline calls `detect` once on the first frame and treats a
/// `None` result as "no region yet" (feed returns without opening a segment).
/// C++ mirrors this with `std::optional<Region>`: `nullopt` means detection has
/// not resolved a region for this frame.
///
/// Non-copyable polymorphic base (prevents slicing). Move is allowed so
/// concrete test doubles can be returned by value.
struct IDetector {
  IDetector() = default;
  IDetector(const IDetector&) = delete;
  IDetector& operator=(const IDetector&) = delete;
  IDetector(IDetector&&) = default;
  IDetector& operator=(IDetector&&) = default;
  virtual ~IDetector() = default;

  /// Detect the subtitle region for `frame`. `nullopt` when no region is
  /// resolved (pipeline defers opening a segment).
  [[nodiscard]] virtual std::optional<Region> detect(const Frame& frame) = 0;
};

}  // namespace sublift
