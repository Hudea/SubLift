#include <sublift/engine/session.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include <sublift/core/color.hpp>
#include <sublift/engine/analysis_executor.hpp>
#include <sublift/engine/segment_recognizer.hpp>
#include <sublift/text/dedupe.hpp>
#include <sublift/timing/changepoint.hpp>
#include <sublift/timing/timeline.hpp>

namespace sublift {

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

} // namespace

class ExtractionSession::Impl {
 public:
  Impl(IFrameSource& source, IOcrEngine& ocr, IDetector* detector, ISubtitleSink* sink,
       ExtractionConfig config)
      : source_(source),
        ocr_(ocr),
        detector_(detector),
        sink_(sink),
        config_(std::move(config)),
        timing_config_(timing::TimingConfig{
            .hysteresis_ms = config_.hysteresis_ms,
            .confirm_ms = config_.confirm_ms,
            .patrol_interval_ms = config_.patrol_interval_ms,
            .change_threshold = config_.change_threshold,
            .presence_threshold = config_.presence_threshold,
            .enable_ssim_patrol = config_.enable_ssim_patrol,
            .ssim_patrol_threshold = config_.ssim_patrol_threshold,
            .aspect_ratio_threshold = config_.aspect_ratio_threshold,
            .ocr_anchor_delay_ms = config_.ocr_anchor_delay_ms,
            .ocr_consensus_frames = config_.ocr_consensus_frames,
            .pure_timing = config_.pure_timing,
            .retain_midpoint_in_pure_timing = config_.retain_midpoint_in_pure_timing,
        }),
        detector_machine_(timing_config_),
        timeline_(timing_config_),
        analysis_executor_(std::make_unique<AnalysisExecutor>(
            timing_config_, config_.num_threads, config_.queue_capacity)),
        segment_recognizer_(std::make_unique<SegmentRecognizer>(
            ocr_, config_, nullptr)) {
    start_time_ = std::chrono::steady_clock::now();
    consumer_thread_ = std::thread(&Impl::consumer_loop, this);
  }

  ~Impl() {
    cancel();
    stop_and_join_threads();
  }

  void set_observer(IExtractionObserver* observer) {
    observer_ = observer;
    if (segment_recognizer_) {
      segment_recognizer_->set_observer(observer);
    }
  }

  void cancel() {
    bool already = cancelled_.exchange(true, std::memory_order_acq_rel);
    if (already) {
      return;
    }
    running_.store(false, std::memory_order_release);
    source_.cancel();
    if (in_process_video_.load(std::memory_order_acquire) ||
        active_feed_threads_.load(std::memory_order_acquire) > 0) {
      stop_and_join_threads();
    } else {
      try {
        drain();
      } catch (...) {}
    }
  }

  void drain() {
    if (analysis_executor_) {
      analysis_executor_->finish_pushing();
    }
    {
      std::lock_guard<std::mutex> lock(consumer_join_mutex_);
      if (consumer_thread_.joinable() &&
          consumer_thread_.get_id() != std::this_thread::get_id()) {
        try {
          consumer_thread_.join();
        } catch (...) {}
      }
    }
    check_and_rethrow();
    if (analysis_executor_) {
      analysis_executor_->check_and_rethrow();
    }
    if (segment_recognizer_) {
      segment_recognizer_->drain();
      check_and_rethrow();
    }
  }

  [[nodiscard]] bool is_running() const noexcept {
    return running_.load(std::memory_order_acquire);
  }

  [[nodiscard]] bool is_cancelled() const noexcept {
    return cancelled_.load(std::memory_order_acquire);
  }

  [[nodiscard]] ExtractionMetrics metrics() const noexcept {
    std::lock_guard<std::mutex> lock(metrics_mutex_);
    ExtractionMetrics m = metrics_;
    if (segment_recognizer_) {
      m.total_ocr_calls = segment_recognizer_->ocr_calls_count();
    }
    m.total_segments_detected = timeline_.total_closed_segments();
    return m;
  }

  [[nodiscard]] const ExtractionConfig& config() const noexcept {
    return config_;
  }

  void feed(Frame frame, bool is_pre_cropped = false) {
    if (finalized_.load(std::memory_order_acquire) ||
        is_cancelled() ||
        stopped_.load(std::memory_order_acquire)) {
      return;
    }

    struct ActiveFeedGuard {
      std::atomic<int>& count;
      explicit ActiveFeedGuard(std::atomic<int>& c) : count(c) {
        count.fetch_add(1, std::memory_order_acq_rel);
      }
      ~ActiveFeedGuard() {
        count.fetch_sub(1, std::memory_order_acq_rel);
      }
    } feed_guard{active_feed_threads_};

    struct ExceptionGuard {
      Impl* self;
      int initial_exceptions{std::uncaught_exceptions()};
      ~ExceptionGuard() {
        if (std::uncaught_exceptions() > initial_exceptions) {
          self->running_.store(false, std::memory_order_release);
          self->signal_stop();
        }
      }
    } guard{this};

    check_and_rethrow();
    if (analysis_executor_) {
      analysis_executor_->check_and_rethrow();
    }
    if (segment_recognizer_) {
      segment_recognizer_->check_and_rethrow();
    }

    if (frame.image.empty() || frame.image.width() <= 0 || frame.image.height() <= 0) {
      return;
    }

    bool expected_not_running = false;
    if (running_.compare_exchange_strong(expected_not_running, true, std::memory_order_acq_rel)) {
      std::lock_guard<std::mutex> lock(metrics_mutex_);
      start_time_ = std::chrono::steady_clock::now();
    }
    auto current_last = last_feed_timestamp_.load(std::memory_order_relaxed);
    while (frame.timestamp_ms > current_last &&
           !last_feed_timestamp_.compare_exchange_weak(
               current_last, frame.timestamp_ms, std::memory_order_release, std::memory_order_relaxed)) {
    }
    {
      std::lock_guard<std::mutex> lock(metrics_mutex_);
      metrics_.total_frames_processed += 1;
    }

    ImageView view = frame.image.view();
    ImageBuffer roi_buf;
    if (is_pre_cropped) {
      // Frame was pre-cropped by source extraction plan; do not double-crop!
      roi_buf = std::move(frame.image);
    } else if (detector_ != nullptr) {
      auto detected = detector_->detect(frame);
      if (detected.has_value()) {
        const auto& b = detected->box;
        if (b.width <= 0 || b.height <= 0) {
          return;
        }
        int x0 = std::clamp(b.x, 0, view.width());
        int y0 = std::clamp(b.y, 0, view.height());
        int x1 = std::clamp(b.x + b.width, 0, view.width());
        int y1 = std::clamp(b.y + b.height, 0, view.height());
        int w = x1 - x0;
        int h = y1 - y0;
        if (w <= 0 || h <= 0) {
          return;
        }
        roi_buf = copy_view_to_buffer(view.roi(x0, y0, w, h));
      } else {
        return;
      }
    } else if (config_.region_bottom_ratio > 0.0 && config_.region_bottom_ratio < 1.0) {
      int h = static_cast<int>(view.height() * config_.region_bottom_ratio);
      if (h > 0 && h <= view.height()) {
        int y = view.height() - h;
        roi_buf = copy_view_to_buffer(view.roi(0, y, view.width(), h));
      } else {
        roi_buf = std::move(frame.image);
      }
    } else {
      roi_buf = std::move(frame.image);
    }

    if (observer_ != nullptr) {
      std::int64_t total_ms = (total_duration_ms_ > 0) ? total_duration_ms_ : frame.timestamp_ms;
      observer_->on_progress(frame.timestamp_ms, total_ms, config_.sample_fps);
    }

    uint64_t push_idx = next_push_index_.fetch_add(1, std::memory_order_acq_rel);
    analysis_executor_->push(push_idx, frame.timestamp_ms, std::move(roi_buf));
  }

  std::vector<SubtitleEntry> finalize() {
    finalized_.store(true, std::memory_order_release);
    running_.store(false, std::memory_order_release);

    struct RunningGuard {
      std::atomic<bool>& flag;
      ~RunningGuard() { flag.store(false, std::memory_order_release); }
    } guard{running_};

    if (is_cancelled()) {
      check_and_rethrow();
      stop_and_join_threads();
      return {};
    }

    // 1. Signal AnalysisExecutor that frame ingestion is done and join consumer
    analysis_executor_->finish_pushing();
    {
      std::lock_guard<std::mutex> lock(consumer_join_mutex_);
      if (consumer_thread_.joinable() &&
          consumer_thread_.get_id() != std::this_thread::get_id()) {
        try {
          consumer_thread_.join();
        } catch (...) {}
      }
    }
    check_and_rethrow();
    analysis_executor_->check_and_rethrow();

    if (is_cancelled()) {
      return {};
    }

    // 2. Finalize timeline to close any open trailing segment
    timeline_.finalize(last_feed_timestamp_.load(std::memory_order_acquire));
    while (auto seg = timeline_.pop_closed_segment()) {
      if (observer_ != nullptr) {
        observer_->on_segment_detected(*seg);
      }
      std::uint64_t entry_id = ++next_entry_id_;
      segment_recognizer_->submit(std::move(*seg), entry_id);
    }

    // 3. Drain SegmentRecognizer OCR worker queue
    segment_recognizer_->drain();
    check_and_rethrow();

    auto raw_entries = segment_recognizer_->take_raw_entries();

    auto final_entries = text::merge_entries(
        raw_entries, config_.merge_gap_ms, config_.min_duration_ms, config_.drop_empty_text);

    {
      std::lock_guard<std::mutex> lock(metrics_mutex_);
      metrics_.total_entries_produced = final_entries.size();
      metrics_.total_segments_detected = timeline_.total_closed_segments();
      metrics_.total_ocr_calls = segment_recognizer_->ocr_calls_count();

      std::int64_t elapsed = 0;
      if (start_time_ != std::chrono::steady_clock::time_point{}) {
        elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - start_time_)
                      .count();
        if (elapsed < 0) {
          elapsed = 0;
        }
      }
      metrics_.wall_time_ms = elapsed;
      if (elapsed > 0 && metrics_.total_frames_processed > 0) {
        metrics_.effective_fps =
            (static_cast<double>(metrics_.total_frames_processed) * 1000.0) /
            static_cast<double>(elapsed);
      } else {
        metrics_.effective_fps = 0.0;
      }
    }

    for (const auto& entry : final_entries) {
      if (sink_ != nullptr) {
        sink_->emit(entry);
      }
    }

    if (sink_ != nullptr) {
      sink_->flush();
      sink_->close();
    }
    if (observer_ != nullptr) {
      observer_->on_complete(metrics_);
    }

    return final_entries;
  }

  std::vector<SubtitleEntry> process_video(
      const std::filesystem::path& video_path,
      const std::optional<SourceBox>& region_box) {
    if (is_cancelled()) {
      return {};
    }
    running_.store(true, std::memory_order_release);
    in_process_video_.store(true, std::memory_order_release);
    start_time_ = std::chrono::steady_clock::now();

    struct ProcessVideoGuard {
      std::atomic<bool>& flag;
      ~ProcessVideoGuard() { flag.store(false, std::memory_order_release); }
    } pv_guard{in_process_video_};

    struct RunningGuard {
      std::atomic<bool>& flag;
      ~RunningGuard() { flag.store(false, std::memory_order_release); }
    } guard{running_};

    if (is_cancelled()) return {};
    VideoInfo info = source_.probe(video_path);
    if (is_cancelled()) return {};
    total_duration_ms_ = info.duration_ms;
    FrameIOPlan plan = source_.plan(info, region_box);
    if (is_cancelled()) return {};
    const bool pre_cropped = plan.output_crop.has_value();

    source_.extract(video_path, config_.sample_fps, plan.output_crop,
                    [this, pre_cropped](Frame frame) {
                      feed(std::move(frame), pre_cropped);
                      return !is_cancelled();
                    });

    return finalize();
  }

 private:
  void consumer_loop() {
    while (true) {
      try {
        auto frame_opt = analysis_executor_->pop();
        if (!frame_opt.has_value()) {
          break;
        }
        consume_analyzed(*frame_opt);
      } catch (...) {
        record_error(std::current_exception());
        break;
      }
    }
  }

  void consume_analyzed(const timing::AnalyzedFrame& analyzed) {
    auto event = detector_machine_.process_analyzed_frame(analyzed);
    if (event.has_value()) {
      timeline_.consume(*event, analyzed.roi_image.view(), analyzed.timestamp_ms);
      while (auto seg = timeline_.pop_closed_segment()) {
        if (observer_ != nullptr) {
          observer_->on_segment_detected(*seg);
        }
        std::uint64_t entry_id = ++next_entry_id_;
        segment_recognizer_->submit(std::move(*seg), entry_id);
      }
    } else {
      timeline_.on_stable_frame(analyzed.roi_image.view(), analyzed.timestamp_ms);
    }
  }

  void record_error(std::exception_ptr ep) noexcept {
    {
      std::lock_guard<std::mutex> lock(error_mutex_);
      if (!error_ptr_) {
        error_ptr_ = ep;
      }
    }
    running_.store(false, std::memory_order_release);
    signal_stop();
  }

  void signal_stop() noexcept {
    stopped_.store(true, std::memory_order_release);
    if (analysis_executor_) {
      analysis_executor_->cancel();
    }
    if (segment_recognizer_) {
      segment_recognizer_->cancel();
    }
  }

  void stop_and_join_threads() noexcept {
    signal_stop();
    std::lock_guard<std::mutex> lock(consumer_join_mutex_);
    if (consumer_thread_.joinable() &&
        consumer_thread_.get_id() != std::this_thread::get_id()) {
      try {
        consumer_thread_.join();
      } catch (...) {}
    }
  }

  void check_and_rethrow() {
    std::lock_guard<std::mutex> lock(error_mutex_);
    if (error_ptr_) {
      std::exception_ptr ep = error_ptr_;
      error_ptr_ = nullptr;
      std::rethrow_exception(ep);
    }
  }

  IFrameSource& source_;
  IOcrEngine& ocr_;
  IDetector* detector_{nullptr};
  ISubtitleSink* sink_{nullptr};

  ExtractionConfig config_;
  IExtractionObserver* observer_{nullptr};

  timing::TimingConfig timing_config_;
  timing::ChangePointDetector detector_machine_;
  timing::TimelineBuilder timeline_;

  std::unique_ptr<AnalysisExecutor> analysis_executor_;
  std::unique_ptr<SegmentRecognizer> segment_recognizer_;

  std::atomic<bool> running_{false};
  std::atomic<bool> cancelled_{false};
  std::atomic<bool> finalized_{false};
  std::atomic<bool> stopped_{false};

  mutable std::mutex metrics_mutex_;
  ExtractionMetrics metrics_{};
  std::chrono::steady_clock::time_point start_time_{};
  std::atomic<std::int64_t> last_feed_timestamp_{0};
  std::int64_t total_duration_ms_{0};

  std::atomic<std::uint64_t> next_push_index_{0};
  std::atomic<std::uint64_t> next_entry_id_{0};
  std::atomic<int> active_feed_threads_{0};
  std::atomic<bool> in_process_video_{false};

  std::mutex error_mutex_;
  std::exception_ptr error_ptr_{nullptr};

  std::mutex consumer_join_mutex_;
  std::thread consumer_thread_;
};

ExtractionSession::ExtractionSession(
    IFrameSource& frame_source,
    IOcrEngine& ocr_engine,
    IDetector* detector,
    ISubtitleSink* sink,
    ExtractionConfig config)
    : impl_(std::make_unique<Impl>(frame_source, ocr_engine, detector, sink, std::move(config))) {}

ExtractionSession::~ExtractionSession() = default;

void ExtractionSession::set_observer(IExtractionObserver* observer) {
  impl_->set_observer(observer);
}

std::vector<SubtitleEntry> ExtractionSession::process_video(
    const std::filesystem::path& video_path,
    const std::optional<SourceBox>& region_box) {
  return impl_->process_video(video_path, region_box);
}

void ExtractionSession::feed(Frame frame) {
  impl_->feed(std::move(frame));
}

std::vector<SubtitleEntry> ExtractionSession::finalize() {
  return impl_->finalize();
}

void ExtractionSession::cancel() {
  impl_->cancel();
}

bool ExtractionSession::is_running() const noexcept {
  return impl_->is_running();
}

bool ExtractionSession::is_cancelled() const noexcept {
  return impl_->is_cancelled();
}

ExtractionMetrics ExtractionSession::metrics() const noexcept {
  return impl_->metrics();
}

const ExtractionConfig& ExtractionSession::config() const noexcept {
  return impl_->config();
}

} // namespace sublift
