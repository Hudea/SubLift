#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

#include <sublift/core/image_buffer.hpp>
#include <sublift/timing/frame_analysis.hpp>
#include <sublift/timing/timing_config.hpp>

namespace sublift {

/**
 * @brief Manages parallel execution of visual feature analysis with strict sequential reordering.
 *
 * AnalysisExecutor takes raw ROI image buffers, distributes visual feature extraction across
 * worker threads, bounds memory via backpressure queues, and yields AnalyzedFrames in strictly
 * monotonically increasing frame index order.
 */
class AnalysisExecutor {
 public:
  AnalysisExecutor(timing::TimingConfig timing_config,
                   std::size_t num_threads,
                   std::size_t queue_capacity);
  ~AnalysisExecutor();

  AnalysisExecutor(const AnalysisExecutor&) = delete;
  AnalysisExecutor& operator=(const AnalysisExecutor&) = delete;
  AnalysisExecutor(AnalysisExecutor&&) noexcept;
  AnalysisExecutor& operator=(AnalysisExecutor&&) noexcept;

  /// Push frame for analysis with backpressure. Blocks if pending tasks >= queue_capacity.
  void push(std::uint64_t frame_index, std::int64_t timestamp_ms, ImageBuffer roi_image);

  /// Pop sequentially ordered AnalyzedFrame. Blocks until next sequential frame is ready,
  /// or returns std::nullopt if finished and drained, or cancelled.
  [[nodiscard]] std::optional<timing::AnalyzedFrame> pop();

  /// Signal that no more frames will be pushed; workers drain remaining tasks.
  void finish_pushing();

  /// Cancel processing and unblock all waiting threads immediately.
  void cancel();

  /// Check and rethrow any exceptions raised in worker threads.
  void check_and_rethrow();

 private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace sublift
