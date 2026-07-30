#include "sublift/changepoint.hpp"

#include <algorithm>
#include <utility>

namespace sublift {

ChangePointDetector::ChangePointDetector(ChangePointConfig config)
    : config_(std::move(config)) {}

void ChangePointDetector::reset() noexcept {
  state_ = State::Empty;
  anchor_signature_.reset();
  anchor_crop_.reset();
  last_signature_.reset();
  consecutive_presence_ = 0;
  consecutive_absence_ = 0;
  stable_start_ms_.reset();
  first_presence_ms_.reset();
  first_absence_ms_.reset();
  change_candidate_ms_.reset();
  patrol_counter_ = 0;
}

void ChangePointDetector::store_anchor_crop(const ImageView* crop) {
  if (crop == nullptr || crop->empty()) {
    anchor_crop_.reset();
    return;
  }
  // Deep copy so the detector owns pixels for later SSIM/patrol.
  ImageBuffer buf(crop->width(), crop->height(), crop->format());
  // Contiguous copy row-by-row to respect source stride.
  const int bpp = bytes_per_pixel(crop->format());
  for (std::int32_t y = 0; y < crop->height(); ++y) {
    const auto* src = crop->row(y);
    auto* dst = buf.data() + static_cast<std::size_t>(y) *
                                 static_cast<std::size_t>(buf.stride_bytes());
    std::copy(src, src + static_cast<std::size_t>(crop->width()) *
                             static_cast<std::size_t>(bpp),
              dst);
  }
  anchor_crop_ = std::move(buf);
}

std::optional<StateEvent> ChangePointDetector::process(
    const FrameSignature& signature, const ImageView* crop) {
  std::optional<StateEvent> event;
  if (state_ == State::Empty) {
    event = process_empty(signature, crop);
  } else {
    event = process_stable(signature, crop);
  }
  last_signature_ = signature;
  return event;
}

std::optional<StateEvent> ChangePointDetector::process_empty(
    const FrameSignature& signature, const ImageView* crop) {
  const bool has_subtitle =
      signature.foreground_ratio >= config_.presence_threshold;

  if (has_subtitle) {
    if (consecutive_presence_ == 0) {
      first_presence_ms_ = signature.timestamp_ms;
    }
    ++consecutive_presence_;
    consecutive_absence_ = 0;

    if (consecutive_presence_ >= config_.hysteresis_frames) {
      // Python: first_presence_ms or signature.timestamp_ms
      // (0 is falsy — historical quirk; do not "fix" with value_or alone)
      const std::int64_t start_ms =
          (first_presence_ms_.has_value() && *first_presence_ms_ != 0)
              ? *first_presence_ms_
              : signature.timestamp_ms;
      state_ = State::Stable;
      anchor_signature_ = signature;
      store_anchor_crop(crop);
      stable_start_ms_ = start_ms;
      consecutive_presence_ = 0;
      first_presence_ms_.reset();
      return StateEvent{EventType::In, start_ms, std::nullopt};
    }
  } else {
    consecutive_presence_ = 0;
    first_presence_ms_.reset();
  }
  return std::nullopt;
}

std::optional<StateEvent> ChangePointDetector::process_stable(
    const FrameSignature& signature, const ImageView* crop) {
  const bool has_subtitle =
      signature.foreground_ratio >= config_.presence_threshold;

  if (!has_subtitle) {
    return handle_disappearance(signature);
  }

  consecutive_absence_ = 0;
  first_absence_ms_.reset();

  if (anchor_signature_.has_value()) {
    return handle_content_change(signature, crop);
  }
  return std::nullopt;
}

std::optional<StateEvent> ChangePointDetector::handle_disappearance(
    const FrameSignature& signature) {
  if (consecutive_absence_ == 0) {
    first_absence_ms_ = signature.timestamp_ms;
  }
  ++consecutive_absence_;

  if (consecutive_absence_ >= config_.hysteresis_frames) {
    // Python: first_absence_ms or signature.timestamp_ms (0 is falsy).
    const std::int64_t end_ms =
        (first_absence_ms_.has_value() && *first_absence_ms_ != 0)
            ? *first_absence_ms_
            : signature.timestamp_ms;
    state_ = State::Empty;
    consecutive_absence_ = 0;
    first_absence_ms_.reset();
    stable_start_ms_.reset();
    change_candidate_ms_.reset();
    anchor_signature_.reset();
    anchor_crop_.reset();
    return StateEvent{EventType::Out, end_ms, std::nullopt};
  }
  return std::nullopt;
}

std::optional<StateEvent> ChangePointDetector::handle_content_change(
    const FrameSignature& signature, const ImageView* crop) {
  const int distance =
      hamming_distance(signature.dhash, anchor_signature_->dhash);

  if (distance <= config_.change_threshold) {
    return handle_patrol_path(signature, crop, distance);
  }

  if (is_ssim_vetoed(crop)) {
    change_candidate_ms_.reset();
    return std::nullopt;
  }

  if (!change_candidate_ms_.has_value()) {
    change_candidate_ms_ = signature.timestamp_ms;
  }

  if (is_new_content_stable(signature)) {
    const std::int64_t change_ms = *change_candidate_ms_;
    change_candidate_ms_.reset();
    state_ = State::StablePrime;
    anchor_signature_ = signature;
    store_anchor_crop(crop);
    stable_start_ms_ = change_ms;
    return StateEvent{EventType::Change, change_ms, change_ms};
  }
  return std::nullopt;
}

std::optional<StateEvent> ChangePointDetector::handle_patrol_path(
    const FrameSignature& signature, const ImageView* crop, int /*distance*/) {
  ++patrol_counter_;

  if (config_.enable_ssim_patrol && change_candidate_ms_.has_value() &&
      is_new_content_stable(signature)) {
    const std::int64_t change_ms = *change_candidate_ms_;
    change_candidate_ms_.reset();
    patrol_counter_ = 0;
    state_ = State::StablePrime;
    anchor_signature_ = signature;
    store_anchor_crop(crop);
    stable_start_ms_ = change_ms;
    return StateEvent{EventType::Change, change_ms, change_ms};
  }

  const auto patrol_ssim = maybe_patrol(signature, crop);
  if (patrol_ssim.has_value() &&
      *patrol_ssim < config_.ssim_patrol_threshold &&
      !change_candidate_ms_.has_value()) {
    change_candidate_ms_ = signature.timestamp_ms;
    return std::nullopt;
  }

  return std::nullopt;
}

std::optional<double> ChangePointDetector::maybe_patrol(
    const FrameSignature& /*signature*/, const ImageView* crop) {
  if (!config_.enable_ssim_patrol) {
    return std::nullopt;
  }
  if (patrol_counter_ < config_.ssim_patrol_interval) {
    return std::nullopt;
  }
  if (!anchor_crop_.has_value() || crop == nullptr || crop->empty()) {
    return std::nullopt;
  }

  patrol_counter_ = 0;
  return compute_foreground_ssim(
      *crop, anchor_crop_->view(), SignatureConfig{},
      config_.ssim_patrol_use_mask, config_.ssim_window_size);
}

bool ChangePointDetector::is_ssim_vetoed(const ImageView* crop) const {
  if (!config_.enable_ssim_verify) {
    return false;
  }
  if (!anchor_crop_.has_value() || crop == nullptr || crop->empty()) {
    return false;
  }
  const double ssim_val =
      compute_ssim(*crop, anchor_crop_->view(), config_.ssim_window_size);
  return ssim_val > config_.ssim_threshold;
}

bool ChangePointDetector::is_new_content_stable(
    const FrameSignature& signature) const {
  if (!last_signature_.has_value()) {
    return false;
  }
  const int last_distance =
      hamming_distance(signature.dhash, last_signature_->dhash);
  return last_distance <= config_.change_threshold;
}

}  // namespace sublift
