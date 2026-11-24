#include <sublift/engine/segment_recognizer.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <exception>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <sublift/text/cleaning.hpp>
#include <sublift/text/consensus.hpp>
#include <sublift/text/line_select.hpp>

namespace sublift {

struct OcrTask {
  timing::Segment segment;
  std::uint64_t entry_id{0};
};

class SegmentRecognizer::Impl {
 public:
  Impl(IOcrEngine& ocr_engine,
       const ExtractionConfig& config,
       IExtractionObserver* observer)
      : ocr_engine_(ocr_engine),
        config_(config),
        observer_(observer),
        queue_capacity_(std::clamp<std::size_t>(config.queue_capacity / 2, 1, 4)),
        subtitle_profile_(config.predetected_profile) {
    if (subtitle_profile_.has_value() &&
        !config_.subtitle_script.empty() && config_.subtitle_script != SCRIPT_AUTO) {
      subtitle_profile_->script = config_.subtitle_script;
    }
    worker_ = std::thread(&Impl::worker_loop, this);
  }

  void set_observer(IExtractionObserver* observer) noexcept {
    observer_ = observer;
  }

  ~Impl() {
    cancel();
    if (worker_.joinable()) {
      try {
        worker_.join();
      } catch (...) {}
    }
  }

  void submit(timing::Segment segment, std::uint64_t entry_id) {
    check_and_rethrow();

    std::unique_lock<std::mutex> lock(mutex_);
    cv_push_.wait(lock, [&] {
      return stopped_ || cancelled_ || task_queue_.size() < queue_capacity_;
    });

    if (stopped_ || cancelled_) {
      return;
    }

    task_queue_.push(OcrTask{std::move(segment), entry_id});
    cv_pop_.notify_one();
  }

  void drain() {
    {
      std::unique_lock<std::mutex> lock(mutex_);
      draining_ = true;
      cv_pop_.notify_all();
      cv_drain_.wait(lock, [&] {
        return stopped_ || cancelled_ || (task_queue_.empty() && !is_processing_);
      });
    }

    if (worker_.joinable()) {
      worker_.join();
    }

    check_and_rethrow();
  }

  void cancel() {
    bool already = cancelled_.exchange(true, std::memory_order_acq_rel);
    if (already) return;

    {
      std::lock_guard<std::mutex> lock(mutex_);
      stopped_.store(true, std::memory_order_release);
      while (!task_queue_.empty()) {
        task_queue_.pop();
      }
    }
    cv_push_.notify_all();
    cv_pop_.notify_all();
    cv_drain_.notify_all();
  }

  [[nodiscard]] std::vector<SubtitleEntry> take_raw_entries() {
    std::lock_guard<std::mutex> lock(mutex_);
    return std::move(raw_entries_);
  }

  [[nodiscard]] std::uint64_t ocr_calls_count() const noexcept {
    return ocr_calls_.load(std::memory_order_relaxed);
  }

  void check_and_rethrow() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (error_ptr_) {
      std::exception_ptr ep = error_ptr_;
      error_ptr_ = nullptr;
      std::rethrow_exception(ep);
    }
  }

 private:
  void worker_loop() {
    while (true) {
      OcrTask task;
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cv_pop_.wait(lock, [&] {
          return stopped_ || cancelled_ || (draining_ && task_queue_.empty()) || !task_queue_.empty();
        });

        if (cancelled_ || (stopped_ && task_queue_.empty())) {
          return;
        }

        if (task_queue_.empty()) {
          if (draining_) {
            return;
          }
          continue;
        }

        task = std::move(task_queue_.front());
        task_queue_.pop();
        is_processing_ = true;
        cv_push_.notify_all();
      }

      try {
        SubtitleEntry entry = recognize_segment(task.segment, task.entry_id);

        // Immediately release pixel memory from the segment
        task.segment.anchor_image = ImageBuffer{};
        task.segment.fallback_images.clear();

        if (observer_ != nullptr) {
          observer_->on_subtitle_entry(entry);
        }

        {
          std::lock_guard<std::mutex> lock(mutex_);
          raw_entries_.push_back(std::move(entry));
          is_processing_ = false;
        }
        cv_drain_.notify_all();
      } catch (...) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!error_ptr_) {
          error_ptr_ = std::current_exception();
        }
        stopped_ = true;
        is_processing_ = false;
        cv_push_.notify_all();
        cv_drain_.notify_all();
        return;
      }
    }
  }

  SubtitleEntry recognize_segment(const timing::Segment& seg, std::uint64_t entry_id) {
    std::string text;
    double conf = 0.0;
    if (config_.pure_timing || seg.anchor_image.empty()) {
      // Pure timing or empty anchor: no OCR
    } else {
      if (!subtitle_profile_.has_value()) {
        subtitle_profile_ = SubtitleProfile::from_crop(
            seg.anchor_image.width(), seg.anchor_image.height(), config_.subtitle_script);
      }
      const SubtitleProfile& profile = *subtitle_profile_;

      int cap = std::max(1, config_.ocr_consensus_frames);
      std::vector<const ImageBuffer*> frames;
      frames.push_back(&seg.anchor_image);
      for (const auto& fb : seg.fallback_images) {
        if (static_cast<int>(frames.size()) >= cap) break;
        if (!fb.empty()) {
          frames.push_back(&fb);
        }
      }

      std::vector<std::pair<std::string, double>> samples;

      for (const auto* frame : frames) {
        ocr_calls_.fetch_add(1, std::memory_order_relaxed);
        OcrResult res = ocr_engine_.recognize(frame->view());
        std::string frame_text;
        double frame_conf = 0.0;
        if (!res.lines.empty() && config_.enable_line_select) {
          auto chosen = text::select_line(
              res.lines, profile, config_.line_select_min_score,
              config_.line_select_min_script, config_.aspect_ratio_threshold);
          if (chosen.has_value()) {
            frame_text = text::cleanup_subtitle_text(chosen->text, profile.script);
            frame_conf = chosen->confidence;
          }
        } else {
          frame_text = text::cleanup_subtitle_text(res.text, profile.script);
          frame_conf = res.confidence;
        }

        if (text::normalize_ocr_text(frame_text).empty()) {
          continue;
        }
        samples.emplace_back(frame_text, frame_conf);

        text::ConsensusResult partial = text::consensus_text(samples, profile.script);

        if (text::should_early_terminate_consensus(
                partial, frame_text, frame_conf, profile,
                config_.confidence_threshold, config_.low_conf_threshold,
                config_.line_select_min_script)) {
          break;
        }
      }

      text::ConsensusResult consensus = text::consensus_text(samples, profile.script);
      if (!consensus.text.empty()) {
        std::string cleaned_text = text::cleanup_subtitle_text(consensus.text, profile.script);
        double confidence = consensus.confidence;
        if (!cleaned_text.empty()) {
          const bool accept = text::should_accept_text(
              cleaned_text, confidence, profile, config_.confidence_threshold,
              config_.low_conf_threshold, consensus.support_votes);
          if (accept) {
            text = std::move(cleaned_text);
            conf = confidence;
          } else {
            conf = confidence;
          }
        }
      }
    }

    return SubtitleEntry{
        .id = entry_id,
        .range = TimeRange{seg.start_ms, seg.end_ms},
        .text = std::move(text),
        .confidence = conf,
    };
  }

  IOcrEngine& ocr_engine_;
  ExtractionConfig config_;
  IExtractionObserver* observer_{nullptr};
  std::size_t queue_capacity_{16};

  std::optional<SubtitleProfile> subtitle_profile_{std::nullopt};
  std::atomic<std::uint64_t> ocr_calls_{0};

  std::mutex mutex_;
  std::condition_variable cv_push_;
  std::condition_variable cv_pop_;
  std::condition_variable cv_drain_;
  std::queue<OcrTask> task_queue_;
  std::vector<SubtitleEntry> raw_entries_;

  std::atomic<bool> stopped_{false};
  std::atomic<bool> cancelled_{false};
  bool draining_{false};
  bool is_processing_{false};

  std::exception_ptr error_ptr_{nullptr};
  std::thread worker_;
};

SegmentRecognizer::SegmentRecognizer(
    IOcrEngine& ocr_engine,
    const ExtractionConfig& config,
    IExtractionObserver* observer)
    : impl_(std::make_unique<Impl>(ocr_engine, config, observer)) {}

SegmentRecognizer::~SegmentRecognizer() = default;
SegmentRecognizer::SegmentRecognizer(SegmentRecognizer&&) noexcept = default;
SegmentRecognizer& SegmentRecognizer::operator=(SegmentRecognizer&&) noexcept = default;

void SegmentRecognizer::set_observer(IExtractionObserver* observer) {
  impl_->set_observer(observer);
}

void SegmentRecognizer::submit(timing::Segment segment, std::uint64_t entry_id) {
  impl_->submit(std::move(segment), entry_id);
}

void SegmentRecognizer::drain() {
  impl_->drain();
}

void SegmentRecognizer::cancel() {
  impl_->cancel();
}

std::vector<SubtitleEntry> SegmentRecognizer::take_raw_entries() {
  return impl_->take_raw_entries();
}

std::uint64_t SegmentRecognizer::ocr_calls_count() const noexcept {
  return impl_->ocr_calls_count();
}

void SegmentRecognizer::check_and_rethrow() {
  impl_->check_and_rethrow();
}

} // namespace sublift
