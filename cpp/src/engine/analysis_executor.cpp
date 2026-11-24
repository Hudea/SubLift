#include <sublift/engine/analysis_executor.hpp>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <vector>

#if defined(__APPLE__)
#include <pthread/qos.h>
#include <sys/sysctl.h>
#endif

namespace sublift {

struct Task {
  std::uint64_t index{0};
  std::int64_t timestamp_ms{0};
  ImageBuffer roi_image;
};

class AnalysisExecutor::Impl {
 public:
  Impl(timing::TimingConfig timing_config,
       std::size_t num_threads,
       std::size_t queue_capacity)
      : timing_config_(timing_config),
        num_threads_(resolve_num_threads(num_threads)),
        queue_capacity_(std::max<std::size_t>(1, queue_capacity)) {
    start_workers();
  }

  ~Impl() {
    cancel();
    stop_and_join();
  }

  static std::size_t resolve_num_threads(std::size_t num_threads) noexcept {
    if (num_threads > 0) {
      return num_threads;
    }
    if (const char* env = std::getenv("SUBLIFT_NUM_THREADS");
        env != nullptr && *env != '\0') {
      char* end = nullptr;
      const long parsed = std::strtol(env, &end, 10);
      if (end != env && *end == '\0' && parsed > 0) {
        return static_cast<std::size_t>(std::clamp(parsed, 1L, 64L));
      }
    }
#if defined(__APPLE__)
    // 优先绑定 P-core（性能核）数量，避免 E-core 拖慢有序重排队列并防止 N*M 上下文抖动
    int perf_cpus = 0;
    std::size_t len = sizeof(perf_cpus);
    if (sysctlbyname("hw.perflevel0.physicalcpu", &perf_cpus, &len, nullptr, 0) == 0 &&
        perf_cpus > 0) {
      return std::clamp<std::size_t>(static_cast<std::size_t>(perf_cpus), 2, 8);
    }
#endif
    const unsigned int hc = std::thread::hardware_concurrency();
    if (hc == 0) {
      return 1;
    }
    if (hc <= 2) {
      return static_cast<std::size_t>(hc);
    }
    return std::clamp<std::size_t>(static_cast<std::size_t>(hc) / 2, 2, 8);
  }

  void push(std::uint64_t frame_index, std::int64_t timestamp_ms, ImageBuffer roi_image) {
    if (finished_pushing_.load(std::memory_order_acquire) ||
        stopped_.load(std::memory_order_acquire) ||
        cancelled_.load(std::memory_order_acquire)) {
      return;
    }
    check_and_rethrow();

    std::unique_lock<std::mutex> lock(task_mutex_);
    task_cv_push_.wait(lock, [&] {
      return stopped_.load(std::memory_order_acquire) ||
             cancelled_.load(std::memory_order_acquire) ||
             finished_pushing_.load(std::memory_order_acquire) ||
             (next_push_index_.load(std::memory_order_acquire) <
              next_consume_index_.load(std::memory_order_acquire) + queue_capacity_);
    });

    if (stopped_.load(std::memory_order_acquire) ||
        cancelled_.load(std::memory_order_acquire) ||
        finished_pushing_.load(std::memory_order_acquire)) {
      check_and_rethrow();
      return;
    }

    uint64_t push_idx = frame_index;
    if (push_idx >= next_push_index_.load(std::memory_order_acquire)) {
      next_push_index_.store(push_idx + 1, std::memory_order_release);
    }
    task_queue_.push(Task{
        .index = push_idx,
        .timestamp_ms = timestamp_ms,
        .roi_image = std::move(roi_image),
    });
    task_cv_pop_.notify_one();
  }

  [[nodiscard]] std::optional<timing::AnalyzedFrame> pop() {
    check_and_rethrow();

    std::unique_lock<std::mutex> lock(reorder_mutex_);
    reorder_cv_.wait(lock, [&] {
      if (stopped_.load(std::memory_order_acquire) ||
          cancelled_.load(std::memory_order_acquire)) {
        return true;
      }
      std::uint64_t consume_idx = next_consume_index_.load(std::memory_order_acquire);
      if (completed_slots_.find(consume_idx) != completed_slots_.end()) {
        return true;
      }
      if (finished_pushing_.load(std::memory_order_acquire)) {
        if (consume_idx >= next_push_index_.load(std::memory_order_acquire)) {
          return true;
        }
        std::lock_guard<std::mutex> tlock(task_mutex_);
        if (task_queue_.empty() && active_workers_.load(std::memory_order_acquire) == 0) {
          return true;
        }
      }
      return false;
    });

    if (cancelled_.load(std::memory_order_acquire)) {
      return std::nullopt;
    }

    if (stopped_.load(std::memory_order_acquire)) {
      check_and_rethrow();
      return std::nullopt;
    }

    std::uint64_t consume_idx = next_consume_index_.load(std::memory_order_acquire);
    auto it = completed_slots_.find(consume_idx);
    if (it != completed_slots_.end()) {
      timing::AnalyzedFrame analyzed = std::move(it->second);
      completed_slots_.erase(it);
      next_consume_index_.fetch_add(1, std::memory_order_release);
      task_cv_push_.notify_all();
      return analyzed;
    }

    if (finished_pushing_.load(std::memory_order_acquire)) {
      if (!completed_slots_.empty()) {
        auto min_it = completed_slots_.begin();
        for (auto cur = completed_slots_.begin(); cur != completed_slots_.end(); ++cur) {
          if (cur->first < min_it->first) {
            min_it = cur;
          }
        }
        timing::AnalyzedFrame analyzed = std::move(min_it->second);
        next_consume_index_.store(min_it->first + 1, std::memory_order_release);
        completed_slots_.erase(min_it);
        task_cv_push_.notify_all();
        return analyzed;
      }
      check_and_rethrow();
      return std::nullopt;
    }

    check_and_rethrow();
    return std::nullopt;
  }

  void finish_pushing() {
    {
      std::lock_guard<std::mutex> lock(task_mutex_);
      finished_pushing_.store(true, std::memory_order_release);
    }
    task_cv_push_.notify_all();
    task_cv_pop_.notify_all();
    {
      std::lock_guard<std::mutex> lock(reorder_mutex_);
    }
    reorder_cv_.notify_all();
  }

  void cancel() {
    bool already = cancelled_.exchange(true, std::memory_order_acq_rel);
    if (already) return;

    stopped_.store(true, std::memory_order_release);
    {
      std::lock_guard<std::mutex> lock(task_mutex_);
      while (!task_queue_.empty()) {
        task_queue_.pop();
      }
    }
    task_cv_push_.notify_all();
    task_cv_pop_.notify_all();
    reorder_cv_.notify_all();
  }

  void check_and_rethrow() {
    std::lock_guard<std::mutex> lock(error_mutex_);
    if (error_ptr_) {
      std::exception_ptr ep = error_ptr_;
      error_ptr_ = nullptr;
      std::rethrow_exception(ep);
    }
  }

 private:
  void start_workers() {
    workers_.reserve(num_threads_);
    for (std::size_t i = 0; i < num_threads_; ++i) {
      workers_.emplace_back(&Impl::worker_loop, this);
    }
  }

  void record_error(std::exception_ptr ep) noexcept {
    {
      std::lock_guard<std::mutex> lock(error_mutex_);
      if (!error_ptr_) {
        error_ptr_ = ep;
      }
    }
    stopped_.store(true, std::memory_order_release);
    task_cv_push_.notify_all();
    task_cv_pop_.notify_all();
    reorder_cv_.notify_all();
  }

  void stop_and_join() noexcept {
    stopped_.store(true, std::memory_order_release);
    task_cv_push_.notify_all();
    task_cv_pop_.notify_all();
    reorder_cv_.notify_all();

    std::lock_guard<std::mutex> lock(join_mutex_);
    for (auto& w : workers_) {
      if (w.joinable()) {
        try {
          w.join();
        } catch (...) {}
      }
    }
    workers_.clear();
  }

  void worker_loop() {
#if defined(__APPLE__)
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INITIATED, 0);
#endif
    while (true) {
      Task task;
      {
        std::unique_lock<std::mutex> lock(task_mutex_);
        task_cv_pop_.wait(lock, [&] {
          return stopped_.load(std::memory_order_acquire) ||
                 cancelled_.load(std::memory_order_acquire) ||
                 (finished_pushing_.load(std::memory_order_acquire) && task_queue_.empty()) ||
                 !task_queue_.empty();
        });

        if (stopped_.load(std::memory_order_acquire) ||
            cancelled_.load(std::memory_order_acquire)) {
          return;
        }

        if (task_queue_.empty()) {
          if (finished_pushing_.load(std::memory_order_acquire)) {
            return;
          }
          continue;
        }

        task = std::move(task_queue_.front());
        task_queue_.pop();
      }

      active_workers_.fetch_add(1, std::memory_order_acq_rel);
      struct ActiveWorkerGuard {
        std::atomic<std::size_t>& count;
        std::condition_variable& cv;
        ~ActiveWorkerGuard() {
          count.fetch_sub(1, std::memory_order_acq_rel);
          cv.notify_all();
        }
      } guard{active_workers_, reorder_cv_};

      try {
        timing::AnalyzedFrame analyzed = timing::analyze_frame(
            std::move(task.roi_image), timing_config_, task.index, task.timestamp_ms);

        {
          std::lock_guard<std::mutex> lock(reorder_mutex_);
          if (stopped_.load(std::memory_order_acquire) ||
              cancelled_.load(std::memory_order_acquire)) {
            return;
          }
          completed_slots_.emplace(analyzed.frame_index, std::move(analyzed));
        }
        reorder_cv_.notify_all();
      } catch (...) {
        record_error(std::current_exception());
        return;
      }
    }
  }

  timing::TimingConfig timing_config_;
  std::size_t num_threads_{1};
  std::size_t queue_capacity_{16};

  std::atomic<bool> stopped_{false};
  std::atomic<bool> cancelled_{false};
  std::atomic<bool> finished_pushing_{false};
  std::atomic<std::size_t> active_workers_{0};

  std::mutex task_mutex_;
  std::condition_variable task_cv_push_;
  std::condition_variable task_cv_pop_;
  std::queue<Task> task_queue_;
  std::atomic<std::uint64_t> next_push_index_{0};
  std::atomic<std::uint64_t> next_consume_index_{0};

  std::mutex reorder_mutex_;
  std::condition_variable reorder_cv_;
  std::unordered_map<std::uint64_t, timing::AnalyzedFrame> completed_slots_;

  std::mutex error_mutex_;
  std::exception_ptr error_ptr_{nullptr};

  std::vector<std::thread> workers_;
  std::mutex join_mutex_;
};

AnalysisExecutor::AnalysisExecutor(
    timing::TimingConfig timing_config,
    std::size_t num_threads,
    std::size_t queue_capacity)
    : impl_(std::make_unique<Impl>(timing_config, num_threads, queue_capacity)) {}

AnalysisExecutor::~AnalysisExecutor() = default;
AnalysisExecutor::AnalysisExecutor(AnalysisExecutor&&) noexcept = default;
AnalysisExecutor& AnalysisExecutor::operator=(AnalysisExecutor&&) noexcept = default;

void AnalysisExecutor::push(std::uint64_t frame_index, std::int64_t timestamp_ms, ImageBuffer roi_image) {
  impl_->push(frame_index, timestamp_ms, std::move(roi_image));
}

std::optional<timing::AnalyzedFrame> AnalysisExecutor::pop() {
  return impl_->pop();
}

void AnalysisExecutor::finish_pushing() {
  impl_->finish_pushing();
}

void AnalysisExecutor::cancel() {
  impl_->cancel();
}

void AnalysisExecutor::check_and_rethrow() {
  impl_->check_and_rethrow();
}

} // namespace sublift
