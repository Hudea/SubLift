#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "sublift/changepoint.hpp"
#include "sublift/config.hpp"
#include "sublift/detector.hpp"
#include "sublift/models.hpp"
#include "sublift/ocr.hpp"
#include "sublift/timeline.hpp"

namespace sublift {

/// Segment-close event (matches Python `SegmentEvent`, frozen dataclass).
///
/// Emitted by `Pipeline::feed` on OUT/CHANGE. Carries the OCR representative
/// frames for `Pipeline::ocr_segment`.
///
/// **Frame ownership:** `anchor_frame` / `fallback_frames` hold *owned* `Frame`
/// values (their `ImageBuffer` deep-copied at retention time). `feed` returns by
/// value, so the caller's input frame may be destroyed afterwards and the event
/// stays valid for OCR. See phase6.2-pipeline.md §5 (P0 ownership contract).
struct SegmentEvent {
  std::int64_t start_ms{0};
  std::int64_t end_ms{0};
  std::optional<Frame> anchor_frame{};
  std::vector<Frame> fallback_frames{};
};

/// Streaming subtitle-extraction orchestrator. Matches `Pipeline` in
/// `src/sublift/pipeline/core.py`.
///
/// Wires detector -> (RGB->BGR quirk) -> signature -> changepoint -> timeline,
/// selecting OCR representative frames (delayed anchor > stable > first) on
/// segment close. The 6.1 parity blocks (signature/changepoint/timeline/
/// dedupe/line_select) are consumed internally.
///
/// **Not thread-safe** (same as the Python pipeline): `feed` / `ocr_segment` /
/// `finalize` / `cancel` must be called serially on one instance.
///
/// **DI lifetime:** `detector` and `ocr` are non-owning references. Both must
/// outlive this `Pipeline` and any deferred `ocr_segment` that still uses
/// them. Destroying an engine while the pipeline holds it is undefined
/// behavior. (Owning injection via `shared_ptr` is deferred to 6.5 worker.)
///
/// **Feed color contract:** input frames must be non-empty RGB24. Feed crops
/// then converts RGB→BGR (Python quirk); non-RGB24 fails with
/// `std::invalid_argument`.
///
/// 6.2 scope: `feed` (feat-06202), `ocr_segment` (feat-06203), `finalize`
/// and `cancel` (feat-06204) are implemented. The end-to-end golden harness
/// lands in 06205. The product CLI/GUI still runs Python.
///
/// Non-copyable / non-movable session object (reference members + stream
/// state must not be casually duplicated).
class Pipeline {
 public:
  Pipeline(IDetector& detector, IOcrEngine& ocr, Config config = {});

  Pipeline(const Pipeline&) = delete;
  Pipeline& operator=(const Pipeline&) = delete;
  Pipeline(Pipeline&&) = delete;
  Pipeline& operator=(Pipeline&&) = delete;

  /// Advance one frame through the timing path. Returns a `SegmentEvent` on
  /// segment close (OUT/CHANGE), otherwise `nullopt`. After `cancel`, returns
  /// `nullopt` and does nothing.
  ///
  /// Requires non-empty RGB24 frame pixels (see class color contract).
  [[nodiscard]] std::optional<SegmentEvent> feed(const Frame& frame);

  /// OCR a closed segment and cache the raw entry. Heavy call (~hundreds of
  /// ms in real engines) - callers should run it off the event loop.
  /// **Not thread-safe** (matches the Python ocr_segment docstring): callers
  /// must serialize ocr_segment() on a single Pipeline instance.
  [[nodiscard]] SubtitleEntry ocr_segment(const SegmentEvent& event);

  /// Close the trailing open segment (if any), OCR it, then return the
  /// dedupe-merged final entry list. Idempotent: a second call returns the
  /// same merged list (closed_entries_ is not cleared). Matches Python
  /// `Pipeline.finalize` (core.py 399-432).
  [[nodiscard]] std::vector<SubtitleEntry> finalize();

  /// Cancel and reset all internal state; release retained frames. After
  /// cancel, feed() permanently returns nullopt (matches Python
  /// `Pipeline.cancel`, core.py 434-446). ocr_segment() may still be called
  /// but, with region_ cleared, takes the region-None branch.
  void cancel();

  /// Current subtitle profile (feat-034b). Matches Python `subtitle_profile`
  /// property. After cancel, reset to `config.subtitle_profile`.
  [[nodiscard]] const std::optional<SubtitleProfile>& subtitle_profile()
      const noexcept {
    return subtitle_profile_;
  }

  [[nodiscard]] std::size_t processed_count() const noexcept {
    return processed_count_;
  }
  [[nodiscard]] std::size_t ocr_call_count() const noexcept {
    return ocr_call_count_;
  }

 private:
  /// Per-segment OCR decision stats (6.2 slimmed form of Python seg_stats).
  /// Perf/timing fields are intentionally omitted - parity does not depend on
  /// them. `early_stop` is a local control-flow marker only, never written to
  /// trace/perf and not exposed.
  struct SegStats {
    int ocr_calls{0};
    int rep_frames{0};
    std::string early_stop;
  };

  // --- segment state helpers (match Python _open_segment etc.) ---
  void ensure_subtitle_profile();
  void open_segment(std::int64_t start_ms, const Frame& frame);
  void on_stable_frame(const Frame& frame);
  void record_sample_frame(const Frame& frame);
  SegmentEvent close_segment(std::int64_t start_ms, std::int64_t end_ms,
                             const Frame* closing_frame,
                             bool clear_open = true);
  void reset_segment_ocr_state() noexcept;

  // --- ocr_segment helpers (feat-06203, match Python _ocr_segment_*) ---
  std::pair<std::string, double> ocr_segment_legacy(const SegmentEvent& event,
                                                    SegStats& stats);
  std::pair<std::string, double> ocr_segment_with_line_select(
      const SegmentEvent& event, SegStats& stats);
  std::pair<std::string, double> ocr_frame_selected(
      const Frame* frame, const SubtitleProfile& profile, SegStats& stats);
  std::pair<std::string, double> ocr_frame_raw(const Frame* frame,
                                               SegStats& stats);
  std::vector<const Frame*> collect_ocr_frames(const SegmentEvent& event) const;
  [[nodiscard]] bool needs_ocr_retry(const std::string& text,
                                     double confidence) const;

  // --- feed helpers ---
  [[nodiscard]] ImageView crop_to_region(const Frame& frame,
                                         const FrameLocalBox& box) const;
  [[nodiscard]] ImageBuffer convert_rgb_to_bgr(const ImageView& rgb) const;

  /// Deep-copy a borrowed frame into an owned one (P0 ownership).
  [[nodiscard]] static Frame clone_frame(const Frame& f);

  IDetector& detector_;
  IOcrEngine& ocr_;  // consumed by ocr_segment (feat-06203)
  Config config_{};

  ChangePointDetector changepoint_;
  TimelineBuilder timeline_{};
  std::optional<Region> region_{};
  std::unordered_map<std::int64_t, Frame> anchor_frames_{};
  std::optional<std::int64_t> open_segment_start_ms_{};
  int pending_anchor_remaining_{0};
  std::optional<Frame> segment_first_frame_{};
  std::optional<Frame> segment_stable_frame_{};
  std::vector<Frame> segment_sample_frames_{};
  std::optional<SubtitleProfile> subtitle_profile_{};
  std::vector<SubtitleEntry> closed_entries_{};
  std::size_t processed_count_{0};
  std::size_t ocr_call_count_{0};
  std::int64_t last_timestamp_ms_{0};
  bool cancelled_{false};
};

}  // namespace sublift
