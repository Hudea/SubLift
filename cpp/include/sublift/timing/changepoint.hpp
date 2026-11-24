#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <utility>

#include <sublift/core/image_buffer.hpp>
#include <sublift/core/image_view.hpp>
#include <sublift/features/signature.hpp>
#include <sublift/features/ssim.hpp>
#include <sublift/timing/frame_analysis.hpp>
#include <sublift/timing/timing_config.hpp>

namespace sublift::timing {

enum class State : std::uint8_t {
  Empty = 0,
  Stable = 1,
  StablePrime = 2,
};

enum class EventType : std::uint8_t {
  In = 0,
  Out = 1,
  Change = 2,
};

struct StateEvent {
  EventType type{EventType::In};
  std::int64_t timestamp_ms{0};
  std::optional<std::int64_t> prev_end_ms{std::nullopt};

  friend bool operator==(const StateEvent&, const StateEvent&) = default;
};

struct ChangeCandidate {
  std::int64_t timestamp_ms{0};
  features::FrameSignature signature{};
  std::optional<ImageBuffer> crop{};
  std::optional<ImageBuffer> mask{};
  std::int32_t stable_frames{1};
  std::int64_t stable_duration_ms{0};
};

struct FrameEvalCache {
  struct Entry {
    const void* ptr_a{nullptr};
    const void* ptr_b{nullptr};
    double mask_ssim{-1.0};           // -1.0 = unset; [0.0, 1.0] = valid
    double crop_ssim{-1.0};           // -1.0 = unset; [0.0, 1.0] = valid
    std::int8_t two_way_overlap{-1};  // -1 = unset; 0 = false; 1 = true
    double overlap_min_ret{-1.0};
  };

  static constexpr std::size_t kMaxEntries = 4;
  std::array<Entry, kMaxEntries> entries{};
  std::size_t count{0};

  std::size_t ssim_eval_count{0};
  std::size_t ssim_cache_hits{0};
  std::size_t overlap_eval_count{0};
  std::size_t overlap_cache_hits{0};

  void clear_frame() noexcept { count = 0; }
  void reset() noexcept {
    count = 0;
    ssim_eval_count = 0;
    ssim_cache_hits = 0;
    overlap_eval_count = 0;
    overlap_cache_hits = 0;
  }

  [[nodiscard]] Entry* find(const void* pa, const void* pb) noexcept {
    if (pa == nullptr || pb == nullptr) return nullptr;
    if (std::less<const void*>{}(pb, pa)) {
      std::swap(pa, pb);
    }
    for (std::size_t i = 0; i < count; ++i) {
      if (entries[i].ptr_a == pa && entries[i].ptr_b == pb) {
        return &entries[i];
      }
    }
    return nullptr;
  }

  [[nodiscard]] Entry* get_or_create(const void* pa, const void* pb) noexcept {
    if (pa == nullptr || pb == nullptr) return nullptr;
    if (std::less<const void*>{}(pb, pa)) {
      std::swap(pa, pb);
    }
    if (auto* e = find(pa, pb)) {
      return e;
    }
    if (count < kMaxEntries) {
      auto& e = entries[count++];
      e = Entry{pa, pb, -1.0, -1.0, -1, -1.0};
      return &e;
    }
    return nullptr;
  }
};

class ChangePointDetector {
 public:
  explicit ChangePointDetector(TimingConfig config = {});
  ~ChangePointDetector() = default;

  std::optional<StateEvent> process(const features::FrameSignature& sig,
                                    const ImageView& crop = {},
                                    const ImageView& valid_mask = {});

  std::optional<StateEvent> process(const features::FrameSignature& sig,
                                    const ImageView* crop,
                                    const ImageView* valid_mask = nullptr);

  std::optional<StateEvent> process_frame(const ImageView& roi, std::int64_t timestamp_ms);

  std::optional<StateEvent> process_analyzed_frame(const AnalyzedFrame& analyzed);

  void reset() noexcept;

  [[nodiscard]] State current_state() const noexcept { return state_; }
  [[nodiscard]] const TimingConfig& config() const noexcept { return config_; }
  [[nodiscard]] const std::optional<features::FrameSignature>& anchor_signature() const noexcept {
    return anchor_signature_;
  }
  [[nodiscard]] const std::optional<ImageBuffer>& anchor_crop() const noexcept {
    return anchor_crop_;
  }
  [[nodiscard]] const std::optional<ImageBuffer>& anchor_mask() const noexcept {
    return anchor_mask_;
  }
  [[nodiscard]] const std::optional<ChangeCandidate>& change_candidate() const noexcept {
    return change_candidate_;
  }
  [[nodiscard]] std::size_t mask_computation_count() const noexcept {
    return mask_computation_count_;
  }
  [[nodiscard]] const FrameEvalCache& eval_cache() const noexcept { return eval_cache_; }

  [[nodiscard]] double eval_mask_ssim(const ImageView& mask_a, const ImageView& mask_b);
  [[nodiscard]] double eval_crop_ssim(const ImageView& crop_a, const ImageView& crop_b);
  [[nodiscard]] bool eval_two_way_overlap(const ImageView& mask_a, const ImageView& mask_b,
                                          double min_ret = 0.60);

 private:
  struct FrameMaskContext;

  void store_anchor_crop(const ImageView* crop, const ImageView* mask = nullptr);
  void clear_anchor() noexcept;
  [[nodiscard]] bool is_wide_strip(const ImageView& view) const noexcept;
  [[nodiscard]] bool is_wide_frame(const ImageView* crop,
                                   const FrameMaskContext& mask_ctx) const noexcept;
  [[nodiscard]] bool has_wide_anchor() const noexcept;

  [[nodiscard]] std::optional<StateEvent> process_empty(
      const features::FrameSignature& signature, const ImageView* crop,
      FrameMaskContext& mask_ctx);
  [[nodiscard]] std::optional<StateEvent> process_stable(
      const features::FrameSignature& signature, const ImageView* crop,
      FrameMaskContext& mask_ctx);
  [[nodiscard]] std::optional<StateEvent> handle_disappearance(
      const features::FrameSignature& signature, const ImageView* crop,
      const FrameMaskContext& mask_ctx);
  [[nodiscard]] std::optional<StateEvent> handle_content_change(
      const features::FrameSignature& signature, const ImageView* crop,
      FrameMaskContext& mask_ctx);
  [[nodiscard]] std::optional<StateEvent> handle_patrol_path(
      const features::FrameSignature& signature, const ImageView* crop, int distance,
      FrameMaskContext& mask_ctx);
  [[nodiscard]] std::optional<double> maybe_patrol(
      const features::FrameSignature& signature, const ImageView* crop,
      FrameMaskContext& mask_ctx);
  [[nodiscard]] bool is_ssim_vetoed(const ImageView* crop) const;
  [[nodiscard]] ChangeCandidate make_candidate(
      const features::FrameSignature& signature, const ImageView* crop,
      const ImageView* mask = nullptr) const;

  TimingConfig config_;
  State state_{State::Empty};

  std::optional<features::FrameSignature> anchor_signature_{};
  std::optional<ImageBuffer> anchor_crop_{};
  std::optional<ImageBuffer> anchor_mask_{};

  features::SsimWorkspace patrol_workspace_{};

  std::optional<features::FrameSignature> last_signature_{};
  int consecutive_presence_{0};
  int consecutive_absence_{0};
  std::optional<std::int64_t> stable_start_ms_{};
  std::optional<std::int64_t> first_presence_ms_{};
  std::optional<std::int64_t> first_absence_ms_{};
  std::optional<ChangeCandidate> change_candidate_{};
  int patrol_counter_{0};
  std::optional<std::int64_t> last_patrol_ms_{};
  std::size_t mask_computation_count_{0};

  FrameEvalCache eval_cache_{};
};

} // namespace sublift::timing
