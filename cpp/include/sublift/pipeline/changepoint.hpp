#pragma once

#include <cstdint>
#include <optional>

#include "sublift/config.hpp"
#include "sublift/image.hpp"
#include "sublift/signature.hpp"

namespace sublift {

/// Matches Python `State` Enum.name: EMPTY / STABLE / STABLE_PRIME.
enum class State : std::uint8_t {
  Empty = 0,
  Stable = 1,
  StablePrime = 2,
};

/// Matches Python `EventType` Enum.name: IN / OUT / CHANGE.
enum class EventType : std::uint8_t {
  In = 0,
  Out = 1,
  Change = 2,
};

/// Matches Python `StateEvent`.
struct StateEvent {
  EventType event_type{EventType::In};
  std::int64_t timestamp_ms{0};
  /// Set only for CHANGE (equals timestamp_ms in the Python oracle).
  std::optional<std::int64_t> prev_end_ms{};

  friend constexpr bool operator==(const StateEvent&,
                                   const StateEvent&) = default;
};

/// Change-point state machine. Matches `ChangePointDetector` in
/// `src/sublift/pipeline/changepoint.py`. No TraceRecorder.
///
/// `process` accepts an optional crop view for SSIM verify / patrol. When null,
/// those paths no-op (same as Python `crop=None`). On IN/CHANGE the detector
/// deep-copies a non-null crop into an owned buffer for later SSIM.
class ChangePointDetector {
 public:
  explicit ChangePointDetector(ChangePointConfig config = {});

  /// Process one frame. Returns an event when the state machine emits one.
  [[nodiscard]] std::optional<StateEvent> process(
      const FrameSignature& signature, const ImageView* crop = nullptr);

  void reset() noexcept;

  [[nodiscard]] State current_state() const noexcept { return state_; }

 private:
  [[nodiscard]] std::optional<StateEvent> process_empty(
      const FrameSignature& signature, const ImageView* crop);
  [[nodiscard]] std::optional<StateEvent> process_stable(
      const FrameSignature& signature, const ImageView* crop);
  [[nodiscard]] std::optional<StateEvent> handle_disappearance(
      const FrameSignature& signature);
  [[nodiscard]] std::optional<StateEvent> handle_content_change(
      const FrameSignature& signature, const ImageView* crop);
  [[nodiscard]] std::optional<StateEvent> handle_patrol_path(
      const FrameSignature& signature, const ImageView* crop, int distance);
  [[nodiscard]] std::optional<double> maybe_patrol(const FrameSignature& signature,
                                                   const ImageView* crop);
  [[nodiscard]] bool is_ssim_vetoed(const ImageView* crop) const;
  [[nodiscard]] bool is_new_content_stable(const FrameSignature& signature) const;

  void store_anchor_crop(const ImageView* crop);

  ChangePointConfig config_{};
  State state_{State::Empty};
  std::optional<FrameSignature> anchor_signature_{};
  std::optional<ImageBuffer> anchor_crop_{};
  std::optional<FrameSignature> last_signature_{};
  int consecutive_presence_{0};
  int consecutive_absence_{0};
  std::optional<std::int64_t> stable_start_ms_{};
  std::optional<std::int64_t> first_presence_ms_{};
  std::optional<std::int64_t> first_absence_ms_{};
  std::optional<std::int64_t> change_candidate_ms_{};
  int patrol_counter_{0};
};

}  // namespace sublift
