#include <sublift/timing/timeline.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <set>
#include <stdexcept>
#include <utility>

#include <sublift/core/color.hpp>

namespace sublift::timing {

namespace {

ImageBuffer clone_view(const ImageView& view) {
  if (view.empty()) {
    return ImageBuffer{};
  }
  ImageBuffer buf = ImageBuffer::allocate_uninitialized(
      view.width(), view.height(), view.format());
  const int bpp = bytes_per_pixel(view.format());
  const std::size_t row_bytes =
      static_cast<std::size_t>(view.width()) * static_cast<std::size_t>(bpp);
  for (std::int32_t y = 0; y < view.height(); ++y) {
    const auto* src = view.row(y);
    auto* dst = buf.row(y);
    std::memcpy(dst, src, row_bytes);
  }
  return buf;
}

} // namespace

TimelineBuilder::TimelineBuilder(TimingConfig config)
    : config_(config) {}

void TimelineBuilder::reset() {
  segments_.clear();
  total_closed_segments_ = 0;
  open_segment_start_ms_.reset();
  segment_first_frame_.reset();
  segment_stable_frame_.reset();
  segment_delayed_anchor_.reset();
  segment_sample_frames_.clear();
}

void TimelineBuilder::open_segment(std::int64_t start_ms, const ImageView& frame_image,
                                   std::int64_t timestamp_ms) {
  open_segment_start_ms_ = start_ms;
  segment_first_frame_.reset();
  segment_stable_frame_.reset();
  segment_delayed_anchor_.reset();
  segment_sample_frames_.clear();

  if (frame_image.empty()) {
    return;
  }
  if (config_.pure_timing) {
    if (config_.retain_midpoint_in_pure_timing) {
      record_sample_frame(frame_image, timestamp_ms);
    }
    return;
  }

  segment_first_frame_ = FrameEntry{timestamp_ms, clone_view(frame_image)};
  segment_sample_frames_.push_back(FrameEntry{timestamp_ms, clone_view(frame_image)});

  if (config_.ocr_anchor_delay_ms <= 0 || (timestamp_ms - start_ms >= config_.ocr_anchor_delay_ms)) {
    segment_delayed_anchor_ = FrameEntry{timestamp_ms, clone_view(frame_image)};
  }
}

void TimelineBuilder::on_stable_frame(const ImageView& frame_image, std::int64_t timestamp_ms) {
  if (!open_segment_start_ms_.has_value() || frame_image.empty()) {
    return;
  }
  if (config_.pure_timing) {
    if (config_.retain_midpoint_in_pure_timing) {
      record_sample_frame(frame_image, timestamp_ms);
    }
    return;
  }
  segment_stable_frame_ = FrameEntry{timestamp_ms, clone_view(frame_image)};
  if (!segment_delayed_anchor_.has_value()) {
    if (timestamp_ms - *open_segment_start_ms_ >= config_.ocr_anchor_delay_ms) {
      segment_delayed_anchor_ = FrameEntry{timestamp_ms, clone_view(frame_image)};
    }
  }
  record_sample_frame(frame_image, timestamp_ms);
}

void TimelineBuilder::record_sample_frame(const ImageView& frame, std::int64_t timestamp_ms) {
  if (frame.empty()) {
    return;
  }
  if (config_.pure_timing) {
    if (!config_.retain_midpoint_in_pure_timing) {
      return;
    }
    if (!segment_sample_frames_.empty() && segment_sample_frames_.back().timestamp_ms == timestamp_ms) {
      segment_sample_frames_.back() = FrameEntry{timestamp_ms, clone_view(frame)};
      return;
    }
    constexpr std::size_t kPureTimingSampleCap = 16;
    if (segment_sample_frames_.size() >= kPureTimingSampleCap) {
      // 长句超过 16 帧时按偶数索引等距抽稀，在有限内存下均匀覆盖整段起止跨度以命中中点帧
      std::size_t write_idx = 0;
      for (std::size_t i = 0; i < segment_sample_frames_.size(); i += 2) {
        if (write_idx != i) {
          segment_sample_frames_[write_idx] = std::move(segment_sample_frames_[i]);
        }
        ++write_idx;
      }
      segment_sample_frames_.resize(write_idx);
    }
    segment_sample_frames_.push_back(FrameEntry{timestamp_ms, clone_view(frame)});
    return;
  }
  int cap = std::max(1, config_.ocr_consensus_frames);
  if (segment_sample_frames_.empty()) {
    segment_sample_frames_.push_back(FrameEntry{timestamp_ms, clone_view(frame)});
    return;
  }
  if (segment_sample_frames_.back().timestamp_ms == timestamp_ms) {
    segment_sample_frames_.back() = FrameEntry{timestamp_ms, clone_view(frame)};
    return;
  }
  if (static_cast<int>(segment_sample_frames_.size()) < cap) {
    segment_sample_frames_.push_back(FrameEntry{timestamp_ms, clone_view(frame)});
    return;
  }

  // Cap reached: keep first slot, replace tail with latest frame
  segment_sample_frames_.back() = FrameEntry{timestamp_ms, clone_view(frame)};
  if (cap >= 3 && segment_sample_frames_.size() >= 3) {
    std::size_t mid = segment_sample_frames_.size() / 2;
    if (segment_sample_frames_[mid].timestamp_ms < timestamp_ms) {
      if (segment_sample_frames_.size() > 2) {
        // Shift next-newest sample into mid slot to preserve temporal span
        const auto& next_newest = segment_sample_frames_[segment_sample_frames_.size() - 2];
        segment_sample_frames_[mid] = FrameEntry{next_newest.timestamp_ms, clone_view(next_newest.image.view())};
      }
    }
  }
}

void TimelineBuilder::close_segment(std::int64_t start_ms, std::int64_t end_ms,
                                    const ImageView& closing_frame,
                                    std::optional<std::int64_t> closing_ts) {
  if (config_.pure_timing) {
    std::int64_t anchor_ts = start_ms;
    ImageBuffer anchor_img;
    if (config_.retain_midpoint_in_pure_timing) {
      const std::int64_t target_mid_ms = start_ms + (end_ms - start_ms) / 2;
      if (!segment_sample_frames_.empty()) {
        std::size_t best_idx = 0;
        std::int64_t best_dist = std::llabs(
            static_cast<long long>(segment_sample_frames_[0].timestamp_ms - target_mid_ms));
        for (std::size_t i = 1; i < segment_sample_frames_.size(); ++i) {
          const std::int64_t dist = std::llabs(
              static_cast<long long>(segment_sample_frames_[i].timestamp_ms - target_mid_ms));
          if (dist < best_dist) {
            best_dist = dist;
            best_idx = i;
          }
        }
        anchor_ts = segment_sample_frames_[best_idx].timestamp_ms;
        anchor_img = std::move(segment_sample_frames_[best_idx].image);
      } else if (!closing_frame.empty()) {
        anchor_ts = closing_ts.value_or(start_ms);
        anchor_img = clone_view(closing_frame);
      }
    }
    ++total_closed_segments_;
    segments_.push_back(Segment{
        .start_ms = start_ms,
        .end_ms = end_ms,
        .anchor_timestamp_ms = anchor_ts,
        .fallback_timestamps_ms = {},
        .anchor_image = std::move(anchor_img),
        .fallback_images = {},
    });
    open_segment_start_ms_.reset();
    segment_first_frame_.reset();
    segment_stable_frame_.reset();
    segment_delayed_anchor_.reset();
    segment_sample_frames_.clear();
    return;
  }

  // Primary anchor resolution priority: delayed > stable > first
  std::optional<FrameEntry> anchor;
  if (segment_delayed_anchor_.has_value()) {
    anchor = std::move(segment_delayed_anchor_);
  } else if (segment_stable_frame_.has_value()) {
    anchor = std::move(segment_stable_frame_);
  } else if (segment_first_frame_.has_value()) {
    anchor = std::move(segment_first_frame_);
  }

  std::vector<std::int64_t> fallback_ts;
  std::vector<ImageBuffer> fallback_imgs;
  std::set<std::int64_t> seen;
  if (anchor.has_value()) {
    seen.insert(anchor->timestamp_ms);
  }

  for (auto& s : segment_sample_frames_) {
    if (!seen.count(s.timestamp_ms)) {
      seen.insert(s.timestamp_ms);
      fallback_ts.push_back(s.timestamp_ms);
      fallback_imgs.push_back(std::move(s.image));
    }
  }

  auto try_add = [&](std::optional<FrameEntry>& cand) {
    if (cand.has_value() && !seen.count(cand->timestamp_ms)) {
      seen.insert(cand->timestamp_ms);
      fallback_ts.push_back(cand->timestamp_ms);
      fallback_imgs.push_back(std::move(cand->image));
    }
  };

  try_add(segment_stable_frame_);
  try_add(segment_first_frame_);
  if (!closing_frame.empty() && closing_ts.has_value() && !seen.count(*closing_ts)) {
    seen.insert(*closing_ts);
    fallback_ts.push_back(*closing_ts);
    fallback_imgs.push_back(clone_view(closing_frame));
  }
  try_add(segment_delayed_anchor_);

  Segment seg{
      .start_ms = start_ms,
      .end_ms = end_ms,
      .anchor_timestamp_ms = anchor.has_value() ? anchor->timestamp_ms : start_ms,
      .fallback_timestamps_ms = std::move(fallback_ts),
      .anchor_image = anchor.has_value() ? std::move(anchor->image) : ImageBuffer{},
      .fallback_images = std::move(fallback_imgs),
  };
  ++total_closed_segments_;
  segments_.push_back(std::move(seg));

  open_segment_start_ms_.reset();
  segment_first_frame_.reset();
  segment_stable_frame_.reset();
  segment_delayed_anchor_.reset();
  segment_sample_frames_.clear();
}

void TimelineBuilder::consume(const StateEvent& event, const ImageView& frame_image,
                             std::optional<std::int64_t> current_frame_ts) {
  std::int64_t frame_ts = current_frame_ts.value_or(event.timestamp_ms);
  switch (event.type) {
    case EventType::In:
      if (open_segment_start_ms_.has_value()) {
        close_segment(*open_segment_start_ms_, event.timestamp_ms);
      }
      open_segment(event.timestamp_ms, frame_image, frame_ts);
      break;
    case EventType::Out:
      if (open_segment_start_ms_.has_value()) {
        close_segment(*open_segment_start_ms_, event.timestamp_ms, frame_image, frame_ts);
      }
      break;
    case EventType::Change:
      if (!event.prev_end_ms.has_value()) {
        throw std::invalid_argument("TimelineBuilder: CHANGE event requires prev_end_ms");
      }
      if (open_segment_start_ms_.has_value()) {
        close_segment(*open_segment_start_ms_, *event.prev_end_ms);
      }
      open_segment(event.timestamp_ms, frame_image, frame_ts);
      break;
  }
}

void TimelineBuilder::finalize(std::int64_t last_timestamp_ms) {
  if (open_segment_start_ms_.has_value()) {
    close_segment(*open_segment_start_ms_, std::max(*open_segment_start_ms_, last_timestamp_ms));
  }
}

std::optional<Segment> TimelineBuilder::pop_closed_segment() {
  if (segments_.empty()) {
    return std::nullopt;
  }
  Segment seg = std::move(segments_.front());
  segments_.erase(segments_.begin());
  return seg;
}

std::vector<Segment> TimelineBuilder::build() const & {
  std::vector<Segment> out;
  out.reserve(segments_.size());
  for (const auto& s : segments_) {
    out.push_back(s.clone());
  }
  return out;
}

} // namespace sublift::timing
