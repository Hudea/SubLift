#pragma once

#include <cstdint>
#include <optional>
#include <vector>
#include <sublift/core/image_buffer.hpp>
#include <sublift/core/image_view.hpp>
#include <sublift/timing/changepoint.hpp>
#include <sublift/timing/segment.hpp>
#include <sublift/timing/timing_config.hpp>

namespace sublift::timing {

class TimelineBuilder {
 public:
  TimelineBuilder() = default;
  explicit TimelineBuilder(TimingConfig config);
  ~TimelineBuilder() = default;

  void consume(const StateEvent& event, const ImageView& frame_image = {},
               std::optional<std::int64_t> current_frame_ts = std::nullopt);
  void on_stable_frame(const ImageView& frame_image, std::int64_t timestamp_ms);
  void consume_frame(const ImageView& frame_image, std::int64_t timestamp_ms) {
    on_stable_frame(frame_image, timestamp_ms);
  }
  void finalize(std::int64_t last_timestamp_ms);
  void finalize_open_segment(std::int64_t last_timestamp_ms) { finalize(last_timestamp_ms); }

  [[nodiscard]] std::optional<Segment> pop_closed_segment();
  [[nodiscard]] std::size_t total_closed_segments() const noexcept { return total_closed_segments_; }
  [[nodiscard]] const std::vector<Segment>& segments() const noexcept { return segments_; }
  [[nodiscard]] std::vector<Segment> build() && noexcept { return std::move(segments_); }
  [[nodiscard]] std::vector<Segment> build() const &;
  void reset();

 private:
  struct FrameEntry {
    std::int64_t timestamp_ms{0};
    ImageBuffer image{};
  };

  void open_segment(std::int64_t start_ms, const ImageView& frame_image, std::int64_t timestamp_ms);
  void close_segment(std::int64_t start_ms, std::int64_t end_ms, const ImageView& closing_frame = {},
                     std::optional<std::int64_t> closing_ts = std::nullopt);
  void record_sample_frame(const ImageView& frame, std::int64_t timestamp_ms);

  TimingConfig config_{};
  std::size_t total_closed_segments_{0};
  std::vector<Segment> segments_;
  std::optional<std::int64_t> open_segment_start_ms_;

  std::optional<FrameEntry> segment_first_frame_;
  std::optional<FrameEntry> segment_stable_frame_;
  std::optional<FrameEntry> segment_delayed_anchor_;
  std::vector<FrameEntry> segment_sample_frames_;
};

} // namespace sublift::timing
