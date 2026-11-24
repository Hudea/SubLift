#include <sublift/adapters/mock/mock_frame_source.hpp>
#include <cstring>
#include <utility>

namespace sublift::adapters::mock {

MockFrameSource::MockFrameSource(MockFrameSource&& other) noexcept {
  std::scoped_lock lock(other.mutex_);
  info_ = other.info_;
  frames_ = std::move(other.frames_);
  mock_thumbnails_ = std::move(other.mock_thumbnails_);
  sample_call_count_ = other.sample_call_count_;
  last_sample_bottom_ratio_ = other.last_sample_bottom_ratio_;
  last_sample_max_samples_ = other.last_sample_max_samples_;
  cancelled_.store(other.cancelled_.load(std::memory_order_relaxed), std::memory_order_relaxed);
}

MockFrameSource& MockFrameSource::operator=(MockFrameSource&& other) noexcept {
  if (this != &other) {
    std::scoped_lock lock(mutex_, other.mutex_);
    info_ = other.info_;
    frames_ = std::move(other.frames_);
    mock_thumbnails_ = std::move(other.mock_thumbnails_);
    sample_call_count_ = other.sample_call_count_;
    last_sample_bottom_ratio_ = other.last_sample_bottom_ratio_;
    last_sample_max_samples_ = other.last_sample_max_samples_;
    cancelled_.store(other.cancelled_.load(std::memory_order_relaxed), std::memory_order_relaxed);
  }
  return *this;
}

void MockFrameSource::add_frame(Frame frame) {
  std::lock_guard<std::mutex> lock(mutex_);
  frames_.push_back(std::move(frame));
}

void MockFrameSource::set_frames(std::vector<Frame> frames) {
  std::lock_guard<std::mutex> lock(mutex_);
  frames_ = std::move(frames);
}

void MockFrameSource::clear_frames() {
  std::lock_guard<std::mutex> lock(mutex_);
  frames_.clear();
}

std::size_t MockFrameSource::frame_count() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return frames_.size();
}

void MockFrameSource::set_video_info(VideoInfo info) {
  std::lock_guard<std::mutex> lock(mutex_);
  info_ = info;
}

void MockFrameSource::set_keyframe_thumbnails(std::vector<KeyframeThumbnail> thumbnails) {
  std::lock_guard<std::mutex> lock(mutex_);
  mock_thumbnails_ = std::move(thumbnails);
}

void MockFrameSource::add_keyframe_thumbnail(KeyframeThumbnail thumbnail) {
  std::lock_guard<std::mutex> lock(mutex_);
  mock_thumbnails_.push_back(std::move(thumbnail));
}

void MockFrameSource::clear_keyframe_thumbnails() {
  std::lock_guard<std::mutex> lock(mutex_);
  mock_thumbnails_.clear();
}

std::size_t MockFrameSource::sample_call_count() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return sample_call_count_;
}

double MockFrameSource::last_sample_bottom_ratio() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return last_sample_bottom_ratio_;
}

std::size_t MockFrameSource::last_sample_max_samples() const noexcept {
  std::lock_guard<std::mutex> lock(mutex_);
  return last_sample_max_samples_;
}

VideoInfo MockFrameSource::probe(const std::filesystem::path& /*video_path*/) {
  std::lock_guard<std::mutex> lock(mutex_);
  return info_;
}

FrameIOPlan MockFrameSource::plan(
    const VideoInfo& info,
    const std::optional<SourceBox>& requested_box) {
  return FrameIOPlan{
      .output_crop = requested_box,
      .output_mode = requested_box.has_value() ? FrameOutputMode::RoiRgb : FrameOutputMode::FullRgb,
      .source = SourceFrameInfo{
          .width = info.width,
          .height = info.height,
          .display_transform_ok = true,
          .transform_note = std::nullopt,
          .pixel_aspect_num = 1,
          .pixel_aspect_den = 1,
          .relative_video_start_ms = 0,
      },
      .fallback_reason = std::nullopt,
  };
}

void MockFrameSource::extract(
    const std::filesystem::path& /*video_path*/,
    double /*fps*/,
    const std::optional<SourceBox>& /*crop*/,
    std::function<bool(Frame)> consumer) {
  cancelled_.store(false, std::memory_order_relaxed);

  std::vector<Frame> frames_copy;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    frames_copy.reserve(frames_.size());
    for (const auto& f : frames_) {
      frames_copy.push_back(Frame{
          .timestamp_ms = f.timestamp_ms,
          .image = f.image.clone(),
      });
    }
  }

  if (!frames_copy.empty()) {
    for (auto& f : frames_copy) {
      if (cancelled_.load(std::memory_order_relaxed)) {
        break;
      }
      if (!consumer(std::move(f))) {
        break;
      }
    }
    return;
  }

  // Fallback: generate 5 synthetic frames (0ms, 200ms, 400ms, 600ms, 800ms)
  for (int i = 0; i < 5; ++i) {
    if (cancelled_.load(std::memory_order_relaxed)) {
      break;
    }
    Frame f{
        .timestamp_ms = i * 200,
        .image = ImageBuffer(100, 40, PixelFormat::RGB24),
    };
    if (!consumer(std::move(f))) {
      break;
    }
  }
}

ImageBuffer MockFrameSource::extract_frame(
    const std::filesystem::path& /*video_path*/,
    std::int64_t timestamp_ms,
    const std::optional<SourceBox>& /*crop*/) {
  std::lock_guard<std::mutex> lock(mutex_);
  for (const auto& f : frames_) {
    if (f.timestamp_ms == timestamp_ms) {
      return f.image.clone();
    }
  }
  const std::int32_t fallback_w = info_.width > 0 ? info_.width : 100;
  const std::int32_t fallback_h = info_.height > 0 ? info_.height : 40;
  return ImageBuffer(fallback_w, fallback_h, PixelFormat::RGB24);
}

std::vector<KeyframeThumbnail> MockFrameSource::sample_keyframe_thumbnails(
    const std::filesystem::path& /*video_path*/,
    double bottom_ratio,
    std::size_t max_samples) {
  std::lock_guard<std::mutex> lock(mutex_);
  sample_call_count_++;
  last_sample_bottom_ratio_ = bottom_ratio;
  last_sample_max_samples_ = max_samples;

  if (cancelled_.load(std::memory_order_relaxed)) {
    return {};
  }

  std::vector<KeyframeThumbnail> result;
  if (!mock_thumbnails_.empty()) {
    result.reserve(mock_thumbnails_.size());
    for (const auto& thumb : mock_thumbnails_) {
      result.push_back(thumb.clone());
    }
  } else if (!frames_.empty()) {
    result.reserve(frames_.size());
    for (const auto& f : frames_) {
      ImageBuffer gray_buf = ImageBuffer::allocate_uninitialized(320, 80, PixelFormat::Gray8);
      std::memset(gray_buf.data(), 0, 320 * 80);
      result.push_back(KeyframeThumbnail{
          .timestamp_ms = f.timestamp_ms,
          .image = std::move(gray_buf),
      });
    }
  }

  if (max_samples > 0 && result.size() > max_samples) {
    result.resize(max_samples);
  }
  return result;
}

void MockFrameSource::cancel() {
  cancelled_.store(true, std::memory_order_relaxed);
}

bool MockFrameSource::is_cancelled() const noexcept {
  return cancelled_.load(std::memory_order_relaxed);
}

} // namespace sublift::adapters::mock

