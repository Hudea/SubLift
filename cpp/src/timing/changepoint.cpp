#include <sublift/timing/changepoint.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <utility>

#include <sublift/core/color.hpp>
#include <sublift/features/dhash.hpp>
#include <sublift/features/mask.hpp>
#include <sublift/features/ssim.hpp>
#include <sublift/features/visual_analysis.hpp>

namespace sublift::timing {

namespace {

ImageBuffer copy_view_to_buffer(const ImageView& view) {
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

bool masks_overlap_two_way(const ImageView& mask_a, const ImageView& mask_b,
                           double min_ret = 0.60) {
  if (mask_a.width() != mask_b.width() || mask_a.height() != mask_b.height()) {
    return false;
  }
  int total_a = 0, total_b = 0, total_ov = 0;
  for (int y = 0; y < mask_a.height(); ++y) {
    const auto* a_row = mask_a.row(y);
    const auto* b_row = mask_b.row(y);
    for (int x = 0; x < mask_a.width(); ++x) {
      const bool ha = (a_row[x] > 128);
      const bool hb = (b_row[x] > 128);
      if (ha) ++total_a;
      if (hb) ++total_b;
      if (ha && hb) ++total_ov;
    }
  }
  if (total_a == 0 || total_b == 0) return false;
  const double ret_a = static_cast<double>(total_ov) / total_a;
  const double ret_b = static_cast<double>(total_ov) / total_b;
  return (ret_a >= min_ret && ret_b >= min_ret);
}

bool check_sector_glyph_retention_veto(const ImageView& anchor_mask,
                                       const ImageView& cur_mask) noexcept {
  if (anchor_mask.empty() || cur_mask.empty() ||
      anchor_mask.width() != cur_mask.width() ||
      anchor_mask.height() != cur_mask.height()) {
    return false;
  }
  const int h = anchor_mask.height();
  const int w = anchor_mask.width();
  const int l_cut = static_cast<int>(0.25 * w);
  const int r_cut = static_cast<int>(0.75 * w);

  int a_left = 0, c_left = 0, ov_left = 0;
  int a_center = 0, c_center = 0, ov_center = 0;
  int a_right = 0, c_right = 0, ov_right = 0;

  for (int y = 0; y < h; ++y) {
    const auto* a_row = anchor_mask.row(y);
    const auto* c_row = cur_mask.row(y);
    for (int x = 0; x < w; ++x) {
      const bool has_a = (a_row[x] > 128);
      const bool has_c = (c_row[x] > 128);
      if (has_a || has_c) {
        if (x < l_cut) {
          if (has_a) ++a_left;
          if (has_c) ++c_left;
          if (has_a && has_c) ++ov_left;
        } else if (x < r_cut) {
          if (has_a) ++a_center;
          if (has_c) ++c_center;
          if (has_a && has_c) ++ov_center;
        } else {
          if (has_a) ++a_right;
          if (has_c) ++c_right;
          if (has_a && has_c) ++ov_right;
        }
      }
    }
  }

  const int total_a = a_left + a_center + a_right;
  const int total_c = c_left + c_center + c_right;
  const int total_ov = ov_left + ov_center + ov_right;

  if (total_a < 40 || total_c < 40) {
    return false;
  }

  const double ret_a = static_cast<double>(total_ov) / total_a;
  const double ret_c = static_cast<double>(total_ov) / total_c;
  const double size_ratio = static_cast<double>(total_c) / total_a;

  bool sector_changed = false;
  if (a_left >= 40 || c_left >= 40) {
    const double s_a = (a_left > 0) ? (static_cast<double>(ov_left) / a_left) : 0.0;
    const double s_c = (c_left > 0) ? (static_cast<double>(ov_left) / c_left) : 0.0;
    if (s_a < 0.65 || s_c < 0.65) sector_changed = true;
  }
  if (a_center >= 40 || c_center >= 40) {
    const double s_a = (a_center > 0) ? (static_cast<double>(ov_center) / a_center) : 0.0;
    const double s_c = (c_center > 0) ? (static_cast<double>(ov_center) / c_center) : 0.0;
    if (s_a < 0.65 || s_c < 0.65) sector_changed = true;
  }
  if (a_right >= 40 || c_right >= 40) {
    const double s_a = (a_right > 0) ? (static_cast<double>(ov_right) / a_right) : 0.0;
    const double s_c = (c_right > 0) ? (static_cast<double>(ov_right) / c_right) : 0.0;
    if (s_a < 0.65 || s_c < 0.65) sector_changed = true;
  }

  return (!sector_changed && ret_a >= 0.75 && ret_c >= 0.75 &&
          size_ratio >= 0.65 && size_ratio <= 1.50);
}

bool check_patrol_glyph_retention_veto(const ImageView& anchor_mask,
                                       const ImageView& cur_mask) noexcept {
  if (anchor_mask.empty() || cur_mask.empty() ||
      anchor_mask.width() != cur_mask.width() ||
      anchor_mask.height() != cur_mask.height()) {
    return false;
  }
  const int h = anchor_mask.height();
  const int w = anchor_mask.width();
  const int l_cut = static_cast<int>(0.25 * w);
  const int r_cut = static_cast<int>(0.75 * w);

  int total_a = 0, total_c = 0, total_ov = 0;
  int a_center = 0, c_center = 0, ov_center = 0;

  for (int y = 0; y < h; ++y) {
    const auto* a_row = anchor_mask.row(y);
    const auto* c_row = cur_mask.row(y);
    for (int x = 0; x < w; ++x) {
      const bool has_a = (a_row[x] > 128);
      const bool has_c = (c_row[x] > 128);
      if (has_a || has_c) {
        if (has_a) ++total_a;
        if (has_c) ++total_c;
        if (has_a && has_c) ++total_ov;
        if (x >= l_cut && x < r_cut) {
          if (has_a) ++a_center;
          if (has_c) ++c_center;
          if (has_a && has_c) ++ov_center;
        }
      }
    }
  }

  if (total_a < 40 || total_c < 40) {
    return false;
  }

  const double ret_a = static_cast<double>(total_ov) / total_a;
  const double ret_c = static_cast<double>(total_ov) / total_c;
  const double size_ratio = static_cast<double>(total_c) / total_a;
  const double c_ret_a = (a_center > 0) ? (static_cast<double>(ov_center) / a_center) : 0.0;
  const double c_ret_c = (c_center > 0) ? (static_cast<double>(ov_center) / c_center) : 0.0;

  return (ret_a >= 0.75 && ret_c >= 0.75 &&
          size_ratio >= 0.65 && size_ratio <= 1.50 &&
          c_ret_a >= 0.65 && c_ret_c >= 0.65);
}

bool is_usable_mask_view(const ImageView* mask) noexcept {
  return mask != nullptr && !mask->empty() && mask->format() == PixelFormat::Gray8;
}

} // namespace

struct ChangePointDetector::FrameMaskContext {
  const ImageView* crop{nullptr};
  const ImageView* valid_mask{nullptr};
  ChangePointDetector* detector{nullptr};
  mutable std::optional<ImageBuffer> lazy_buffer{};
  mutable ImageView lazy_view{};

  [[nodiscard]] const ImageView* get_mask() const {
    if (is_usable_mask_view(valid_mask)) {
      if (crop == nullptr || crop->empty()) {
        return valid_mask;
      }
      if (valid_mask->width() == crop->width() &&
          valid_mask->height() == crop->height()) {
        return valid_mask;
      }
    }
    if (crop == nullptr || crop->empty()) {
      return nullptr;
    }
    if (!lazy_buffer.has_value()) {
      lazy_buffer = features::compute_binary_mask(*crop);
      lazy_view = lazy_buffer->view();
      if (detector != nullptr) {
        detector->mask_computation_count_++;
      }
    }
    return &lazy_view;
  }
};

ChangePointDetector::ChangePointDetector(TimingConfig config)
    : config_(config), state_(State::Empty) {}

bool ChangePointDetector::is_wide_strip(const ImageView& view) const noexcept {
  return !view.empty() && view.height() > 0 &&
         ((static_cast<double>(view.width()) / static_cast<double>(view.height())) >
          config_.aspect_ratio_threshold);
}

bool ChangePointDetector::is_wide_frame(const ImageView* crop,
                                        const FrameMaskContext& mask_ctx) const noexcept {
  if (crop != nullptr && !crop->empty()) {
    return is_wide_strip(*crop);
  }
  if (is_usable_mask_view(mask_ctx.valid_mask)) {
    return is_wide_strip(*mask_ctx.valid_mask);
  }
  return false;
}

bool ChangePointDetector::has_wide_anchor() const noexcept {
  if (anchor_crop_.has_value()) {
    return is_wide_strip(anchor_crop_->view());
  }
  if (anchor_mask_.has_value()) {
    return is_wide_strip(anchor_mask_->view());
  }
  return false;
}

void ChangePointDetector::reset() noexcept {
  state_ = State::Empty;
  anchor_signature_.reset();
  clear_anchor();
  last_signature_.reset();
  consecutive_presence_ = 0;
  consecutive_absence_ = 0;
  stable_start_ms_.reset();
  first_presence_ms_.reset();
  first_absence_ms_.reset();
  change_candidate_.reset();
  patrol_counter_ = 0;
  last_patrol_ms_.reset();
  mask_computation_count_ = 0;
  eval_cache_.reset();
}

void ChangePointDetector::clear_anchor() noexcept {
  anchor_crop_.reset();
  anchor_mask_.reset();
}

void ChangePointDetector::store_anchor_crop(const ImageView* crop, const ImageView* mask) {
  clear_anchor();
  const bool has_crop = (crop != nullptr && !crop->empty());
  const bool has_mask = is_usable_mask_view(mask);
  if (!has_crop && !has_mask) {
    return;
  }
  if (has_crop) {
    anchor_crop_ = copy_view_to_buffer(*crop);
  }
  if (has_mask && (!has_crop || (mask->width() == crop->width() &&
                                 mask->height() == crop->height()))) {
    anchor_mask_ = copy_view_to_buffer(*mask);
  } else if (has_crop && config_.enable_ssim_patrol && config_.ssim_patrol_use_mask) {
    anchor_mask_ = features::compute_binary_mask(anchor_crop_->view());
    mask_computation_count_++;
  }
}

double ChangePointDetector::eval_mask_ssim(const ImageView& mask_a, const ImageView& mask_b) {
  const void* pa = mask_a.data();
  const void* pb = mask_b.data();
  if (pa != nullptr && pa == pb) {
    eval_cache_.ssim_cache_hits++;
    return 1.0;
  }

  auto* entry = eval_cache_.find(pa, pb);
  if (entry != nullptr && entry->mask_ssim >= 0.0) {
    eval_cache_.ssim_cache_hits++;
    return entry->mask_ssim;
  }

  eval_cache_.ssim_eval_count++;
  const double ssim_val = features::compute_mask_ssim(
      mask_a, mask_b, config_.ssim_window_size, &patrol_workspace_,
      config_.aspect_ratio_threshold);

  if (auto* dest = (entry != nullptr) ? entry : eval_cache_.get_or_create(pa, pb)) {
    dest->mask_ssim = ssim_val;
  }
  return ssim_val;
}

double ChangePointDetector::eval_crop_ssim(const ImageView& crop_a, const ImageView& crop_b) {
  const void* pa = crop_a.data();
  const void* pb = crop_b.data();
  if (pa != nullptr && pa == pb) {
    eval_cache_.ssim_cache_hits++;
    return 1.0;
  }

  auto* entry = eval_cache_.find(pa, pb);
  if (entry != nullptr && entry->crop_ssim >= 0.0) {
    eval_cache_.ssim_cache_hits++;
    return entry->crop_ssim;
  }

  eval_cache_.ssim_eval_count++;
  const double ssim_val = features::compute_ssim(
      crop_a, crop_b, config_.ssim_window_size, &patrol_workspace_);

  if (auto* dest = (entry != nullptr) ? entry : eval_cache_.get_or_create(pa, pb)) {
    dest->crop_ssim = ssim_val;
  }
  return ssim_val;
}

bool ChangePointDetector::eval_two_way_overlap(const ImageView& mask_a, const ImageView& mask_b,
                                              double min_ret) {
  const void* pa = mask_a.data();
  const void* pb = mask_b.data();
  if (pa != nullptr && pa == pb) {
    eval_cache_.overlap_cache_hits++;
    return true;
  }

  auto* entry = eval_cache_.find(pa, pb);
  if (entry != nullptr && entry->two_way_overlap >= 0 && entry->overlap_min_ret == min_ret) {
    eval_cache_.overlap_cache_hits++;
    return entry->two_way_overlap == 1;
  }

  eval_cache_.overlap_eval_count++;
  const bool overlap = masks_overlap_two_way(mask_a, mask_b, min_ret);

  if (auto* dest = (entry != nullptr) ? entry : eval_cache_.get_or_create(pa, pb)) {
    dest->two_way_overlap = overlap ? 1 : 0;
    dest->overlap_min_ret = min_ret;
  }
  return overlap;
}

std::optional<StateEvent> ChangePointDetector::process(
    const features::FrameSignature& sig, const ImageView* crop,
    const ImageView* valid_mask) {
  eval_cache_.clear_frame();
  FrameMaskContext mask_ctx{crop, valid_mask, this};
  std::optional<StateEvent> event;
  if (state_ == State::Empty) {
    event = process_empty(sig, crop, mask_ctx);
  } else {
    event = process_stable(sig, crop, mask_ctx);
  }
  last_signature_ = sig;
  return event;
}

std::optional<StateEvent> ChangePointDetector::process(
    const features::FrameSignature& sig, const ImageView& crop,
    const ImageView& valid_mask) {
  const ImageView* p_crop = crop.empty() ? nullptr : &crop;
  const ImageView* p_mask = valid_mask.empty() ? nullptr : &valid_mask;
  return process(sig, p_crop, p_mask);
}

std::optional<StateEvent> ChangePointDetector::process_frame(
    const ImageView& roi, std::int64_t timestamp_ms) {
  features::MaskConfig mask_cfg{};
  mask_cfg.aspect_ratio_threshold = config_.aspect_ratio_threshold;
  auto analysis = features::compute_visual_analysis(roi, timestamp_ms, mask_cfg);
  ImageView mask_view = analysis.valid_mask.view();
  return process(analysis.signature, &roi, &mask_view);
}

std::optional<StateEvent> ChangePointDetector::process_analyzed_frame(
    const AnalyzedFrame& analyzed) {
  features::FrameSignature sig{
      .timestamp_ms = analyzed.timestamp_ms,
      .foreground_ratio = static_cast<double>(analyzed.presence_ratio),
      .dhash = analyzed.dhash64,
  };
  const ImageView crop_view =
      (config_.pure_timing || analyzed.roi_image.empty())
          ? ImageView{}
          : analyzed.roi_image.view();
  const ImageView mask_view =
      analyzed.roi_mask.empty() ? ImageView{} : analyzed.roi_mask.view();
  const ImageView* p_crop = crop_view.empty() ? nullptr : &crop_view;
  const ImageView* p_mask = mask_view.empty() ? nullptr : &mask_view;
  return process(sig, p_crop, p_mask);
}

std::optional<StateEvent> ChangePointDetector::process_empty(
    const features::FrameSignature& signature, const ImageView* crop,
    FrameMaskContext& mask_ctx) {
  const bool is_wide = is_wide_frame(crop, mask_ctx);
  const bool has_subtitle =
      (signature.foreground_ratio >= config_.presence_threshold) &&
      (!is_wide || signature.dhash != 0ULL);

  if (has_subtitle) {
    if (consecutive_presence_ == 0) {
      first_presence_ms_ = signature.timestamp_ms;
      if (is_wide) {
        store_anchor_crop(crop, mask_ctx.get_mask());
      }
    } else if (last_signature_.has_value()) {
      bool unstable = false;
      const bool center_only =
          ((signature.dhash & 0xFFFFFF0000FFFFFFULL) == 0ULL &&
           (last_signature_->dhash & 0xFFFFFF0000FFFFFFULL) == 0ULL);
      const int stab_thresh = center_only ? 2 : 5;
      const int shared_bits = std::popcount(signature.dhash & last_signature_->dhash);
      if (features::hamming_distance(signature.dhash, last_signature_->dhash) > stab_thresh ||
          (signature.dhash != 0ULL && last_signature_->dhash != 0ULL && shared_bits == 0)) {
        unstable = true;
      }
      if (!unstable && is_wide && anchor_mask_.has_value()) {
        const ImageView* cur_mask = mask_ctx.get_mask();
        if (cur_mask != nullptr && !eval_two_way_overlap(anchor_mask_->view(), *cur_mask, 0.60)) {
          unstable = true;
        }
      }
      if (unstable) {
        first_presence_ms_ = signature.timestamp_ms;
        consecutive_presence_ = 0;
        if (is_wide) {
          store_anchor_crop(crop, mask_ctx.get_mask());
        }
      }
    }
    ++consecutive_presence_;
    consecutive_absence_ = 0;

    bool confirmed = false;
    if (is_wide) {
      const std::int64_t dt =
          last_signature_.has_value()
              ? std::max<std::int64_t>(
                    1, signature.timestamp_ms - last_signature_->timestamp_ms)
              : 0;
      const std::int64_t elapsed_presence =
          (signature.timestamp_ms - *first_presence_ms_) + dt;
      const std::int64_t req_presence =
          (dt <= 120 && config_.hysteresis_ms >= 200) ? 250 : config_.hysteresis_ms;
      confirmed =
          (elapsed_presence >= req_presence) && (consecutive_presence_ >= 2);
    } else {
      const std::int64_t elapsed_presence = signature.timestamp_ms - *first_presence_ms_;
      confirmed = (elapsed_presence >= config_.hysteresis_ms);
    }

    if (confirmed) {
      const std::int64_t start_ms = first_presence_ms_.value_or(signature.timestamp_ms);
      state_ = State::Stable;
      anchor_signature_ = signature;
      store_anchor_crop(crop, mask_ctx.get_mask());
      stable_start_ms_ = start_ms;
      last_patrol_ms_ = start_ms;
      consecutive_presence_ = 0;
      first_presence_ms_.reset();
      return StateEvent{EventType::In, start_ms, std::nullopt};
    }
  } else {
    consecutive_presence_ = 0;
    first_presence_ms_.reset();
    clear_anchor();
  }
  return std::nullopt;
}

std::optional<StateEvent> ChangePointDetector::process_stable(
    const features::FrameSignature& signature, const ImageView* crop,
    FrameMaskContext& mask_ctx) {
  const bool is_wide = is_wide_frame(crop, mask_ctx);
  const bool has_subtitle =
      (signature.foreground_ratio >= config_.presence_threshold) &&
      (!is_wide || signature.dhash != 0ULL);

  if (!has_subtitle) {
    return handle_disappearance(signature, crop, mask_ctx);
  }

  consecutive_absence_ = 0;
  first_absence_ms_.reset();

  if (anchor_signature_.has_value()) {
    return handle_content_change(signature, crop, mask_ctx);
  }
  return std::nullopt;
}

std::optional<StateEvent> ChangePointDetector::handle_disappearance(
    const features::FrameSignature& signature, const ImageView* crop,
    const FrameMaskContext& mask_ctx) {
  if (consecutive_absence_ == 0) {
    first_absence_ms_ = signature.timestamp_ms;
  }
  ++consecutive_absence_;

  const bool is_wide = is_wide_frame(crop, mask_ctx) || has_wide_anchor();
  bool confirmed = false;
  if (is_wide) {
    const std::int64_t dt =
        last_signature_.has_value()
            ? std::max<std::int64_t>(
                  1, signature.timestamp_ms - last_signature_->timestamp_ms)
            : 0;
    const std::int64_t elapsed_absence =
        (signature.timestamp_ms - *first_absence_ms_) + dt;
    const std::int64_t effective_hysteresis_ms =
        (dt <= 120) ? std::min<std::int64_t>(config_.hysteresis_ms, 100)
                    : config_.hysteresis_ms;
    confirmed = (elapsed_absence >= effective_hysteresis_ms);
  } else {
    const std::int64_t elapsed_absence = signature.timestamp_ms - *first_absence_ms_;
    confirmed = (elapsed_absence >= config_.hysteresis_ms);
  }

  if (confirmed) {
    const std::int64_t end_ms = first_absence_ms_.value_or(signature.timestamp_ms);
    state_ = State::Empty;
    consecutive_absence_ = 0;
    first_absence_ms_.reset();
    stable_start_ms_.reset();
    change_candidate_.reset();
    anchor_signature_.reset();
    clear_anchor();
    return StateEvent{EventType::Out, end_ms, std::nullopt};
  }
  return std::nullopt;
}

std::optional<StateEvent> ChangePointDetector::handle_content_change(
    const features::FrameSignature& signature, const ImageView* crop,
    FrameMaskContext& mask_ctx) {
  const int distance =
      features::hamming_distance(signature.dhash, anchor_signature_->dhash);

  bool is_change = (distance > config_.change_threshold);

  const bool is_wide = is_wide_frame(crop, mask_ctx) || has_wide_anchor();
  if (!is_change && config_.enable_ssim_patrol && is_wide) {
    double ssim_val = 1.0;
    if (config_.ssim_patrol_use_mask) {
      if (!anchor_mask_.has_value() && anchor_crop_.has_value()) {
        anchor_mask_ = features::compute_binary_mask(anchor_crop_->view());
        mask_computation_count_++;
      }
      const ImageView* cur_mask = mask_ctx.get_mask();
      if (anchor_mask_.has_value() && cur_mask != nullptr &&
          anchor_mask_->width() == cur_mask->width() &&
          anchor_mask_->height() == cur_mask->height()) {
        ssim_val = eval_mask_ssim(*cur_mask, anchor_mask_->view());
      } else if (crop != nullptr && !crop->empty() && anchor_crop_.has_value() &&
                 anchor_crop_->width() == crop->width() &&
                 anchor_crop_->height() == crop->height()) {
        ssim_val = eval_crop_ssim(*crop, anchor_crop_->view());
      }
    } else if (crop != nullptr && !crop->empty() && anchor_crop_.has_value()) {
      ssim_val = eval_crop_ssim(*crop, anchor_crop_->view());
    }
    if (ssim_val < config_.ssim_patrol_threshold) {
      is_change = true;
    }
  }

  // Sector-Aware Glyph Retention Veto: check if subtitle text is static on screen
  if (is_change && is_wide_frame(crop, mask_ctx)) {
    if (!anchor_mask_.has_value() && anchor_crop_.has_value()) {
      anchor_mask_ = features::compute_binary_mask(anchor_crop_->view());
      mask_computation_count_++;
    }
    const ImageView* cur_mask = mask_ctx.get_mask();

    if (anchor_mask_.has_value() && cur_mask != nullptr &&
        check_sector_glyph_retention_veto(anchor_mask_->view(), *cur_mask)) {
      is_change = false;
      change_candidate_.reset();
    }
  }

  if (!is_change) {
    return handle_patrol_path(signature, crop, distance, mask_ctx);
  }

  if (distance > config_.change_threshold && is_ssim_vetoed(crop)) {
    change_candidate_.reset();
    return std::nullopt;
  }

  const bool was_new_candidate = !change_candidate_.has_value();

  if (was_new_candidate) {
    change_candidate_ = make_candidate(signature, crop, mask_ctx.get_mask());
  }

  auto candidate_visual_match = [&]() -> bool {
    if (!config_.enable_ssim_patrol) {
      return true;
    }
    if (config_.ssim_patrol_use_mask && change_candidate_->mask.has_value()) {
      const ImageView* cur_mask = mask_ctx.get_mask();
      if (cur_mask != nullptr &&
          change_candidate_->mask->width() == cur_mask->width() &&
          change_candidate_->mask->height() == cur_mask->height()) {
        if (is_wide_frame(crop, mask_ctx) &&
            !eval_two_way_overlap(change_candidate_->mask->view(), *cur_mask, 0.60)) {
          return false;
        }
        const double ssim_cand =
            eval_mask_ssim(*cur_mask, change_candidate_->mask->view());
        return ssim_cand >= config_.ssim_patrol_threshold;
      }
    }
    if (crop != nullptr && !crop->empty() &&
        change_candidate_->crop.has_value() &&
        change_candidate_->crop->width() == crop->width() &&
        change_candidate_->crop->height() == crop->height()) {
      const double ssim_cand =
          eval_crop_ssim(*crop, change_candidate_->crop->view());
      return ssim_cand >= config_.ssim_patrol_threshold;
    }
    return true;
  };

  bool confirmed = false;
  const int dist_to_cand =
      features::hamming_distance(signature.dhash, change_candidate_->signature.dhash);
  bool cand_match =
      (dist_to_cand <= std::max(4, config_.change_threshold / 2)) &&
      candidate_visual_match();
  if (cand_match) {
    if (!was_new_candidate) {
      change_candidate_->stable_frames += 1;
    }
    change_candidate_->stable_duration_ms =
        signature.timestamp_ms - change_candidate_->timestamp_ms;
    if (change_candidate_->stable_duration_ms >= config_.confirm_ms - 20) {
      confirmed = true;
    }
  } else {
    if (distance <= config_.change_threshold / 2) {
      change_candidate_.reset();
    } else {
      change_candidate_ = make_candidate(signature, crop, mask_ctx.get_mask());
    }
  }

  if (confirmed) {
    const std::int64_t change_ms = change_candidate_->timestamp_ms;
    change_candidate_.reset();
    state_ = (state_ == State::Stable) ? State::StablePrime : State::Stable;
    anchor_signature_ = signature;
    store_anchor_crop(crop, mask_ctx.get_mask());
    stable_start_ms_ = change_ms;
    last_patrol_ms_ = change_ms;
    return StateEvent{EventType::Change, change_ms, change_ms};
  }
  return std::nullopt;
}

std::optional<StateEvent> ChangePointDetector::handle_patrol_path(
    const features::FrameSignature& signature, const ImageView* crop, int distance,
    FrameMaskContext& mask_ctx) {
  ++patrol_counter_;

  if (change_candidate_.has_value()) {
    const int cand_dist_to_anchor = features::hamming_distance(
        change_candidate_->signature.dhash, anchor_signature_->dhash);
    const int dist_to_cand =
        features::hamming_distance(signature.dhash, change_candidate_->signature.dhash);

    bool reverted_to_anchor = false;
    bool cand_visual_ok = true;

    const ImageView* cur_mask =
        (config_.enable_ssim_patrol && config_.ssim_patrol_use_mask)
            ? mask_ctx.get_mask()
            : nullptr;
    const bool has_mask_patrol =
        config_.enable_ssim_patrol && config_.ssim_patrol_use_mask &&
        anchor_mask_.has_value() && cur_mask != nullptr &&
        anchor_mask_->width() == cur_mask->width() &&
        anchor_mask_->height() == cur_mask->height();
    const bool has_crop_patrol =
        config_.enable_ssim_patrol && crop != nullptr && !crop->empty() &&
        anchor_crop_.has_value();

    if (has_mask_patrol || has_crop_patrol) {
      double ssim_to_anchor = 1.0;
      if (has_mask_patrol) {
        ssim_to_anchor = eval_mask_ssim(*cur_mask, anchor_mask_->view());
      } else {
        ssim_to_anchor = eval_crop_ssim(*crop, anchor_crop_->view());
      }

      if (ssim_to_anchor >= config_.ssim_patrol_threshold) {
        reverted_to_anchor = true;
      }

      if (config_.ssim_patrol_use_mask && change_candidate_->mask.has_value()) {
        if (cur_mask == nullptr) {
          cur_mask = mask_ctx.get_mask();
        }
        if (cur_mask != nullptr &&
            change_candidate_->mask->width() == cur_mask->width() &&
            change_candidate_->mask->height() == cur_mask->height()) {
          if (is_wide_frame(crop, mask_ctx) &&
              !eval_two_way_overlap(change_candidate_->mask->view(), *cur_mask, 0.60)) {
            cand_visual_ok = false;
          } else {
            const double ssim_to_cand =
                eval_mask_ssim(*cur_mask, change_candidate_->mask->view());
            if (ssim_to_cand < config_.ssim_patrol_threshold) {
              cand_visual_ok = false;
            }
          }
        }
      } else if (crop != nullptr && !crop->empty() &&
                 change_candidate_->crop.has_value() &&
                 change_candidate_->crop->width() == crop->width() &&
                 change_candidate_->crop->height() == crop->height()) {
        const double ssim_to_cand =
            eval_crop_ssim(*crop, change_candidate_->crop->view());
        if (ssim_to_cand < config_.ssim_patrol_threshold) {
          cand_visual_ok = false;
        }
      }
    } else if (cand_dist_to_anchor > config_.change_threshold) {
      if (distance <= config_.change_threshold / 2 ||
          dist_to_cand > config_.change_threshold) {
        reverted_to_anchor = true;
      }
    }

    bool confirmed = false;
    if (reverted_to_anchor) {
      change_candidate_.reset();
    } else {
      const bool cand_match =
          (dist_to_cand <= std::max(4, config_.change_threshold / 2)) &&
          cand_visual_ok;
      if (cand_match) {
        change_candidate_->stable_frames += 1;
        change_candidate_->stable_duration_ms =
            signature.timestamp_ms - change_candidate_->timestamp_ms;
        if (change_candidate_->stable_duration_ms >= config_.confirm_ms - 20) {
          confirmed = true;
        }
      } else {
        if (distance <= config_.change_threshold / 2) {
          change_candidate_.reset();
        } else {
          change_candidate_ = make_candidate(signature, crop, mask_ctx.get_mask());
        }
      }
    }

    if (confirmed) {
      const std::int64_t change_ms = change_candidate_->timestamp_ms;
      change_candidate_.reset();
      patrol_counter_ = 0;
      state_ = (state_ == State::Stable) ? State::StablePrime : State::Stable;
      anchor_signature_ = signature;
      store_anchor_crop(crop, mask_ctx.get_mask());
      stable_start_ms_ = change_ms;
      last_patrol_ms_ = change_ms;
      return StateEvent{EventType::Change, change_ms, change_ms};
    }
  }

  const auto patrol_ssim = maybe_patrol(signature, crop, mask_ctx);
  if (patrol_ssim.has_value() &&
      *patrol_ssim < config_.ssim_patrol_threshold &&
      !change_candidate_.has_value()) {
    // Check glyph retention before creating candidate from patrol SSIM drop
    if (anchor_mask_.has_value() && is_wide_frame(crop, mask_ctx)) {
      const ImageView* cur_mask = mask_ctx.get_mask();
      if (cur_mask != nullptr &&
          check_patrol_glyph_retention_veto(anchor_mask_->view(), *cur_mask)) {
        // Anchor subtitle is still static on screen! Veto patrol candidate!
        return std::nullopt;
      }
    }
    change_candidate_ = make_candidate(signature, crop, mask_ctx.get_mask());
    return std::nullopt;
  }

  return std::nullopt;
}

std::optional<double> ChangePointDetector::maybe_patrol(
    const features::FrameSignature& signature, const ImageView* crop,
    FrameMaskContext& mask_ctx) {
  if (!config_.enable_ssim_patrol) {
    return std::nullopt;
  }
  if (last_patrol_ms_.has_value() &&
      (signature.timestamp_ms - *last_patrol_ms_) < config_.patrol_interval_ms) {
    return std::nullopt;
  }

  if (config_.ssim_patrol_use_mask && anchor_mask_.has_value()) {
    const ImageView* current_mask = mask_ctx.get_mask();
    if (current_mask != nullptr &&
        anchor_mask_->width() == current_mask->width() &&
        anchor_mask_->height() == current_mask->height()) {
      patrol_counter_ = 0;
      last_patrol_ms_ = signature.timestamp_ms;
      return eval_mask_ssim(*current_mask, anchor_mask_->view());
    }
  }

  if (!anchor_crop_.has_value() || crop == nullptr || crop->empty()) {
    return std::nullopt;
  }

  patrol_counter_ = 0;
  last_patrol_ms_ = signature.timestamp_ms;
  return eval_crop_ssim(*crop, anchor_crop_->view());
}

bool ChangePointDetector::is_ssim_vetoed(const ImageView* crop) const {
  if (!config_.enable_ssim_verify) {
    return false;
  }
  if (!anchor_crop_.has_value() || crop == nullptr || crop->empty()) {
    return false;
  }
  const double ssim_val =
      features::compute_ssim(*crop, anchor_crop_->view(), config_.ssim_window_size,
                             const_cast<features::SsimWorkspace*>(&patrol_workspace_));
  return ssim_val > config_.ssim_threshold;
}

ChangeCandidate ChangePointDetector::make_candidate(
    const features::FrameSignature& signature, const ImageView* crop,
    const ImageView* mask) const {
  ChangeCandidate cand;
  cand.timestamp_ms = signature.timestamp_ms;
  cand.signature = signature;
  const bool has_crop = (crop != nullptr && !crop->empty());
  const bool has_mask = is_usable_mask_view(mask);
  if (has_crop) {
    cand.crop = copy_view_to_buffer(*crop);
  }
  if (config_.enable_ssim_patrol && config_.ssim_patrol_use_mask) {
    if (has_mask && (!has_crop || (mask->width() == crop->width() &&
                                   mask->height() == crop->height()))) {
      cand.mask = copy_view_to_buffer(*mask);
    } else if (has_crop) {
      cand.mask = features::compute_binary_mask(*crop);
      const_cast<ChangePointDetector*>(this)->mask_computation_count_++;
    }
  }
  cand.stable_frames = 1;
  cand.stable_duration_ms = 0;
  return cand;
}

} // namespace sublift::timing
