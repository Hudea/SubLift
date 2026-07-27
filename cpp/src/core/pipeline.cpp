#include "sublift/pipeline.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <utility>

#include "sublift/dedupe.hpp"
#include "sublift/image.hpp"
#include "sublift/line_select.hpp"
#include "sublift/signature.hpp"

namespace sublift {

Pipeline::Pipeline(IDetector& detector, IOcrEngine& ocr, Config config)
    : detector_(detector),
      ocr_(ocr),
      config_(std::move(config)),
      changepoint_(config_.change_point),
      subtitle_profile_(config_.subtitle_profile) {}

// ---------------------------------------------------------------------------
// feat-06203 / feat-06204 placeholders
// ---------------------------------------------------------------------------

// ---------------------------------------------------------------------------
// ocr_segment (feat-06203) - matches Python ocr_segment (core.py 305-397)
// ---------------------------------------------------------------------------

SubtitleEntry Pipeline::ocr_segment(const SegmentEvent& event) {
  // No region resolved (detector never produced one): emit an empty entry and
  // cache it, mirroring core.py 324-343. No OCR call is made.
  if (!region_.has_value()) {
    SubtitleEntry entry{event.start_ms, event.end_ms, std::string{}, 0.0};
    closed_entries_.push_back(entry);
    return entry;
  }

  SegStats stats;
  std::string text;
  double confidence = 0.0;
  if (config_.enable_line_select) {
    std::tie(text, confidence) = ocr_segment_with_line_select(event, stats);
  } else {
    std::tie(text, confidence) = ocr_segment_legacy(event, stats);
  }

  SubtitleEntry entry{event.start_ms, event.end_ms, text, confidence};
  closed_entries_.push_back(entry);
  return entry;
}

std::pair<std::string, double> Pipeline::ocr_segment_legacy(
    const SegmentEvent& event, SegStats& stats) {
  // Old path: whole-region join + global threshold + single-anchor fallback
  // (core.py 557-587). Perf/trace recording is omitted in 6.2.
  stats.early_stop = "legacy_path";
  const Frame* anchor =
      event.anchor_frame.has_value() ? &*event.anchor_frame : nullptr;
  auto [text, confidence] = ocr_frame_raw(anchor, stats);

  if (needs_ocr_retry(text, confidence)) {
    // seen_ts dedup: anchor (or -1 when absent) plus each fallback's ts.
    std::set<std::int64_t> seen;
    seen.insert(event.anchor_frame.has_value()
                    ? event.anchor_frame->timestamp_ms
                    : static_cast<std::int64_t>(-1));
    for (const auto& fb : event.fallback_frames) {
      if (seen.count(fb.timestamp_ms)) {
        continue;
      }
      seen.insert(fb.timestamp_ms);
      auto [fb_text, fb_conf] = ocr_frame_raw(&fb, stats);
      if (!needs_ocr_retry(fb_text, fb_conf)) {
        text = fb_text;
        confidence = fb_conf;
        break;
      }
      // Keep a non-empty fallback even if it still needs retry, and prefer a
      // higher-confidence non-empty one. Python uses .strip() here; we use the
      // normalize-after-strip check (normalize_ocr_text strips + collapses).
      if (!normalize_ocr_text(fb_text).empty() && normalize_ocr_text(text).empty()) {
        text = fb_text;
        confidence = fb_conf;
      }
      if (fb_conf > confidence && !normalize_ocr_text(fb_text).empty()) {
        text = fb_text;
        confidence = fb_conf;
      }
    }
  }

  if (confidence < config_.confidence_threshold) {
    text.clear();  // blank the text but keep the confidence (Python 585-586)
  }
  return {std::move(text), confidence};
}

std::pair<std::string, double> Pipeline::ocr_segment_with_line_select(
    const SegmentEvent& event, SegStats& stats) {
  // Line-level selection + multi-frame consensus (core.py 589-683).
  ensure_subtitle_profile();
  if (!subtitle_profile_.has_value()) {
    if (region_.has_value()) {
      const auto& box = region_->box;
      subtitle_profile_ =
          SubtitleProfile::from_crop(box.width, box.height, config_.subtitle_script);
    } else {
      stats.early_stop = "no_valid_sample";
      return {std::string{}, 0.0};
    }
  }
  const SubtitleProfile& profile = *subtitle_profile_;

  auto frames = collect_ocr_frames(event);
  stats.rep_frames = static_cast<int>(frames.size());
  if (frames.empty()) {
    stats.early_stop = "no_valid_sample";
    return {std::string{}, 0.0};
  }
  std::vector<std::pair<std::string, double>> samples;
  std::string early_stop = "representative_frames_exhausted";

  for (const Frame* frame : frames) {
    auto [text, conf] = ocr_frame_selected(frame, profile, stats);
    if (normalize_ocr_text(text).empty()) {
      continue;
    }
    samples.emplace_back(text, conf);

    ConsensusResult partial = consensus_text(samples, profile.script);

    // Single high-confidence frame is enough unless CJK+latin are mixed
    // (could be a watermark); keep sampling to let in-segment stability decide.
    const bool mixed_script =
        cjk_ratio(text) > 0.0 && latin_ratio(text) > 0.0;
    if (conf >= config_.confidence_threshold &&
        script_score(text, profile.script) >= config_.line_select_min_script &&
        !(profile.script == SCRIPT_CJK && mixed_script)) {
      early_stop = "single_high_confidence";
      break;
    }
    // >=2 similar-variant votes is enough; do not require exact equality.
    if (partial.support_votes >= 2 &&
        partial.confidence >= config_.low_conf_threshold) {
      early_stop = "two_frame_consensus";
      break;
    }
  }
  stats.early_stop = early_stop;

  ConsensusResult consensus = consensus_text(samples, profile.script);
  if (consensus.text.empty()) {
    return {std::string{}, 0.0};
  }

  std::string text = cleanup_subtitle_text(consensus.text, profile.script);
  double confidence = consensus.confidence;
  if (text.empty()) {
    return {std::string{}, confidence};
  }

  const bool accept = should_accept_text(
      text, confidence, profile, config_.confidence_threshold,
      config_.low_conf_threshold, consensus.support_votes);
  if (!accept) {
    return {std::string{}, confidence};
  }
  return {std::move(text), confidence};
}

std::pair<std::string, double> Pipeline::ocr_frame_selected(
    const Frame* frame, const SubtitleProfile& profile, SegStats& stats) {
  // Single frame -> OCR -> line selection -> (text, conf) (core.py 699-752).
  if (frame == nullptr || !region_.has_value()) {
    return {std::string{}, 0.0};
  }
  ImageView crop = crop_to_region(*frame, region_->box);
  OcrResult result = ocr_.recognize(crop);
  ++stats.ocr_calls;

  if (!result.lines.empty()) {
    auto chosen = select_line(result.lines, profile, config_.line_select_min_score,
                              config_.line_select_min_script);
    if (!chosen.has_value()) {
      return {std::string{}, 0.0};
    }
    std::string text = cleanup_subtitle_text(chosen->text, profile.script);
    return {std::move(text), chosen->confidence};
  }
  // No lines: compat with engines that only fill text/confidence.
  std::string text = cleanup_subtitle_text(result.text, profile.script);
  return {std::move(text), result.confidence};
}

std::pair<std::string, double> Pipeline::ocr_frame_raw(const Frame* frame,
                                                       SegStats& stats) {
  // Single frame -> OCR -> raw (text, conf), no cleanup (core.py 754-780).
  if (frame == nullptr || !region_.has_value()) {
    return {std::string{}, 0.0};
  }
  ImageView crop = crop_to_region(*frame, region_->box);
  OcrResult result = ocr_.recognize(crop);
  ++stats.ocr_calls;
  if (stats.rep_frames == 0) {
    stats.rep_frames = 1;
  }
  return {result.text, result.confidence};
}

std::vector<const Frame*> Pipeline::collect_ocr_frames(
    const SegmentEvent& event) const {
  // anchor + fallbacks, dedup by timestamp_ms, truncate to ocr_consensus_frames
  // (core.py 685-697).
  const int cap = std::max(1, config_.ocr_consensus_frames);
  std::vector<const Frame*> frames;
  std::set<std::int64_t> seen;
  auto try_add = [&](const Frame* fr) {
    if (fr == nullptr) {
      return;
    }
    if (seen.count(fr->timestamp_ms)) {
      return;
    }
    seen.insert(fr->timestamp_ms);
    frames.push_back(fr);
  };
  if (event.anchor_frame.has_value()) {
    try_add(&*event.anchor_frame);
    if (static_cast<int>(frames.size()) >= cap) {
      return frames;
    }
  }
  for (const auto& fb : event.fallback_frames) {
    try_add(&fb);
    if (static_cast<int>(frames.size()) >= cap) {
      break;
    }
  }
  return frames;
}

bool Pipeline::needs_ocr_retry(const std::string& text,
                               double confidence) const {
  // core.py 822-825: retry on empty (stripped) text or sub-threshold conf.
  if (normalize_ocr_text(text).empty()) {
    return true;
  }
  return confidence < config_.confidence_threshold;
}

std::vector<SubtitleEntry> Pipeline::finalize() {
  // Close the trailing open segment, if any (core.py 405-421).
  if (open_segment_start_ms_.has_value()) {
    timeline_.finalize_open_segment(last_timestamp_ms_);
    auto segments = timeline_.build();
    if (!segments.empty()) {
      const auto& last = segments.back();
      if (last.end_ms.has_value()) {
        SegmentEvent event = close_segment(last.start_ms, *last.end_ms,
                                           nullptr, /*clear_open=*/false);
        (void)ocr_segment(event);  // raw entry pushed to closed_entries_
      }
    }
    open_segment_start_ms_.reset();
    reset_segment_ocr_state();
  }
  // Global dedupe (core.py 423-429). closed_entries_ is NOT cleared.
  return merge_entries(closed_entries_, config_.merge_gap_ms,
                       config_.min_duration_ms, config_.drop_empty_text);
}

void Pipeline::cancel() {
  // Matches Python Pipeline.cancel (core.py 434-446). Permanent: feed() will
  // return nullopt forever after this.
  cancelled_ = true;
  changepoint_.reset();
  timeline_.reset();
  region_.reset();
  anchor_frames_.clear();
  closed_entries_.clear();
  open_segment_start_ms_.reset();
  processed_count_ = 0;
  last_timestamp_ms_ = 0;
  reset_segment_ocr_state();
  subtitle_profile_ = config_.subtitle_profile;
}

// ---------------------------------------------------------------------------
// feed (feat-06202) - matches Python Pipeline.feed (core.py 235-303)
// ---------------------------------------------------------------------------

std::optional<SegmentEvent> Pipeline::feed(const Frame& frame) {
  if (cancelled_) {
    return std::nullopt;
  }

  last_timestamp_ms_ = frame.timestamp_ms;
  ++processed_count_;

  // First frame: detect the subtitle region once.
  if (!region_.has_value()) {
    region_ = detector_.detect(frame);
    if (!region_.has_value()) {
      return std::nullopt;
    }
    ensure_subtitle_profile();
  }

  // crop -> RGB->BGR quirk -> signature -> changepoint.
  ImageView crop_view = crop_to_region(frame, region_->box);
  ImageBuffer bgr_buf = convert_rgb_to_bgr(crop_view);
  ImageView bgr_view = bgr_buf.view();

  FrameSignature signature =
      compute_signature(bgr_view, frame.timestamp_ms, config_.signature);
  auto event = changepoint_.process(signature, &bgr_view);

  if (!event.has_value()) {
    on_stable_frame(frame);
    return std::nullopt;
  }

  if (event->event_type == EventType::In) {
    open_segment(event->timestamp_ms, frame);
    timeline_.consume(*event);
    return std::nullopt;
  }

  if (event->event_type == EventType::Out) {
    std::int64_t start_ms =
        open_segment_start_ms_.value_or(event->timestamp_ms);
    timeline_.consume(*event);
    return close_segment(start_ms, event->timestamp_ms, &frame);
  }

  // CHANGE: close the old segment, then open the new one.
  std::int64_t old_start =
      open_segment_start_ms_.value_or(event->timestamp_ms);
  timeline_.consume(*event);
  std::int64_t end_ms =
      event->prev_end_ms.value_or(event->timestamp_ms);
  // closing_frame is null: this frame already shows the new subtitle, so it
  // must not become the old segment's OCR anchor.
  SegmentEvent closed = close_segment(old_start, end_ms, nullptr);
  open_segment(event->timestamp_ms, frame);
  return closed;
}

// ---------------------------------------------------------------------------
// segment state helpers
// ---------------------------------------------------------------------------

void Pipeline::ensure_subtitle_profile() {
  if (subtitle_profile_.has_value() || !region_.has_value()) {
    return;
  }
  const auto& box = region_->box;
  subtitle_profile_ =
      SubtitleProfile::from_crop(box.width, box.height, config_.subtitle_script);
}

void Pipeline::open_segment(std::int64_t start_ms, const Frame& frame) {
  open_segment_start_ms_ = start_ms;
  segment_first_frame_ = clone_frame(frame);
  segment_stable_frame_.reset();
  segment_sample_frames_.clear();
  segment_sample_frames_.push_back(clone_frame(frame));
  int delay = std::max(0, config_.ocr_anchor_delay_frames);
  if (delay == 0) {
    anchor_frames_[start_ms] = clone_frame(frame);
    pending_anchor_remaining_ = 0;
  } else {
    pending_anchor_remaining_ = delay;
  }
}

void Pipeline::on_stable_frame(const Frame& frame) {
  if (!open_segment_start_ms_.has_value()) {
    return;
  }
  if (pending_anchor_remaining_ > 0) {
    --pending_anchor_remaining_;
    if (pending_anchor_remaining_ == 0) {
      anchor_frames_[*open_segment_start_ms_] = clone_frame(frame);
    }
    record_sample_frame(frame);
    return;
  }
  segment_stable_frame_ = clone_frame(frame);
  record_sample_frame(frame);
}

void Pipeline::record_sample_frame(const Frame& frame) {
  int cap = std::max(1, config_.ocr_consensus_frames);
  auto& samples = segment_sample_frames_;
  if (samples.empty()) {
    samples.push_back(clone_frame(frame));
    return;
  }
  if (samples.back().timestamp_ms == frame.timestamp_ms) {
    samples.back() = clone_frame(frame);
    return;
  }
  if (static_cast<int>(samples.size()) < cap) {
    samples.push_back(clone_frame(frame));
    return;
  }
  // Full: keep first, replace last with the new frame.
  samples.back() = clone_frame(frame);
  if (cap >= 3 && samples.size() >= 3) {
    std::size_t mid = samples.size() / 2;
    if (samples[mid].timestamp_ms < frame.timestamp_ms) {
      if (samples.size() > 2) {
        // Move the "next-newest" into the mid slot to preserve time span.
        samples[mid] = clone_frame(samples[samples.size() - 2]);
      }
    }
  }
}

SegmentEvent Pipeline::close_segment(std::int64_t start_ms, std::int64_t end_ms,
                                     const Frame* closing_frame,
                                     bool clear_open) {
  // delayed anchor (locked after the delay countdown), else None.
  std::optional<Frame> delayed;
  if (auto it = anchor_frames_.find(start_ms); it != anchor_frames_.end()) {
    delayed = std::move(it->second);
    anchor_frames_.erase(it);
  }
  std::optional<Frame> first = std::move(segment_first_frame_);
  std::optional<Frame> stable = std::move(segment_stable_frame_);

  // Primary anchor: delayed > stable > first.
  std::optional<Frame> anchor;
  if (delayed.has_value()) {
    anchor = std::move(*delayed);
  } else if (stable.has_value()) {
    anchor = std::move(*stable);
  } else if (first.has_value()) {
    anchor = std::move(*first);
  }

  // Fallbacks: samples + [stable, first, closing, delayed], dedup by ts.
  std::vector<Frame> fallbacks;
  std::set<std::int64_t> seen;
  if (anchor.has_value()) {
    seen.insert(anchor->timestamp_ms);
  }

  auto try_move = [&](std::optional<Frame>& cand) {
    if (!cand.has_value()) {
      return;
    }
    if (seen.count(cand->timestamp_ms)) {
      return;
    }
    seen.insert(cand->timestamp_ms);
    fallbacks.push_back(std::move(*cand));
    cand.reset();
  };

  for (auto& s : segment_sample_frames_) {
    if (seen.count(s.timestamp_ms)) {
      continue;
    }
    seen.insert(s.timestamp_ms);
    fallbacks.push_back(std::move(s));
  }
  try_move(stable);
  try_move(first);
  if (closing_frame != nullptr) {
    if (!seen.count(closing_frame->timestamp_ms)) {
      seen.insert(closing_frame->timestamp_ms);
      fallbacks.push_back(clone_frame(*closing_frame));
    }
  }
  try_move(delayed);

  if (clear_open) {
    open_segment_start_ms_.reset();
    reset_segment_ocr_state();
  }

  return SegmentEvent{start_ms, end_ms, std::move(anchor), std::move(fallbacks)};
}

void Pipeline::reset_segment_ocr_state() noexcept {
  pending_anchor_remaining_ = 0;
  segment_first_frame_.reset();
  segment_stable_frame_.reset();
  segment_sample_frames_.clear();
}

// ---------------------------------------------------------------------------
// feed helpers
// ---------------------------------------------------------------------------

ImageView Pipeline::crop_to_region(const Frame& frame,
                                   const FrameLocalBox& box) const {
  const ImageBuffer& img = frame.image;
  const bool full_local = (box.x == 0 && box.y == 0 &&
                           box.width == img.width() &&
                           box.height == img.height());
  if (full_local) {
    return img.view();  // zero-copy passthrough (feat-038)
  }
  return img.roi(box.x, box.y, box.width, box.height);
}

ImageBuffer Pipeline::convert_rgb_to_bgr(const ImageView& rgb) const {
  // Feed only ever hands us the RGB24 frame crop; fail loudly (including
  // Release/NDEBUG) so a non-RGB source (e.g. Gray8 extractor frame in 6.3)
  // cannot OOB-read in the 3-byte loop below. architecture.md: fail loudly.
  if (rgb.format() != PixelFormat::RGB24 || rgb.width() <= 0 ||
      rgb.height() <= 0) {
    throw std::invalid_argument(
        "Pipeline::feed expects a non-empty RGB24 crop for RGB→BGR conversion");
  }
  // Reproduce the Python feed color quirk: cv2.cvtColor(RGB2BGR) yields a
  // BGR byte order, then compute_signature's _to_gray reads it via
  // COLOR_RGB2GRAY (R/B weights swapped = the documented quirk). C++
  // compute_signature applies COLOR_RGB2GRAY to any 3-channel view regardless
  // of tag, so the bytes must actually be BGR. We swap R<->B by hand so
  // pipeline.cpp stays OpenCV-free; the BGR24 tag is semantically honest.
  ImageBuffer bgr(rgb.width(), rgb.height(), PixelFormat::BGR24);
  for (std::int32_t y = 0; y < rgb.height(); ++y) {
    const std::uint8_t* src = rgb.row(y);
    std::uint8_t* dst = bgr.data() + static_cast<std::size_t>(y) *
                                         static_cast<std::size_t>(bgr.stride_bytes());
    for (std::int32_t x = 0; x < rgb.width(); ++x) {
      dst[x * 3 + 0] = src[x * 3 + 2];  // B <- R
      dst[x * 3 + 1] = src[x * 3 + 1];  // G <- G
      dst[x * 3 + 2] = src[x * 3 + 0];  // R <- B
    }
  }
  return bgr;
}

Frame Pipeline::clone_frame(const Frame& f) {
  return Frame{f.timestamp_ms, f.image.clone()};
}

}  // namespace sublift
