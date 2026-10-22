#pragma once

#include <cmath>
#include <optional>
#include <string>

namespace sublift {

/// Det input scaling policy. The default reproduces RapidOCR's min-side 736
/// rule; a positive pixel budget instead upscales only until width*height
/// reaches the budget and never downscales (ADR-0042 / Phase 18).
struct DetInputGeometry {
  double min_side{736.0};
  double pixel_budget{0.0};
  /// Conditional short-side floor applied together with pixel_budget; 0 keeps
  /// the area-only policy. Used only when real 720p / narrow-band material
  /// reproduces left-edge fragmentation (ADR-0042).
  double short_side{0.0};

  friend constexpr bool operator==(const DetInputGeometry&,
                                   const DetInputGeometry&) = default;
};

/// Product-facing validation for the optional det pixel budget. Zero disables
/// the budget (min-side 736). Positive values must be finite and within a sane
/// range; invalid values are rejected instead of silently falling back.
[[nodiscard]] inline std::optional<std::string> validate_det_short_side(
    double short_side) {
  if (short_side == 0.0) {
    return std::nullopt;
  }
  if (!std::isfinite(short_side)) {
    return "det short side must be finite";
  }
  if (short_side < 64.0 || short_side > 4096.0) {
    return "det short side must be 0 or within [64, 4096]";
  }
  return std::nullopt;
}

[[nodiscard]] inline std::optional<std::string> validate_det_pixel_budget(
    double budget) {
  if (budget == 0.0) {
    return std::nullopt;
  }
  if (!std::isfinite(budget)) {
    return "det pixel budget must be finite";
  }
  if (budget < 10000.0 || budget > 16000000.0) {
    return "det pixel budget must be 0 or within [10000, 16000000]";
  }
  return std::nullopt;
}

}  // namespace sublift
