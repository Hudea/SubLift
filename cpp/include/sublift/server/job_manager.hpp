#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "sublift/models.hpp"

namespace sublift::worker {
class BridgeHandler;
}

namespace sublift::server {

enum class JobStatus {
  Queued,
  Running,
  Completed,
  Failed,
  Cancelled
};

[[nodiscard]] inline std::string to_string(JobStatus status) {
  switch (status) {
    case JobStatus::Queued: return "queued";
    case JobStatus::Running: return "running";
    case JobStatus::Completed: return "completed";
    case JobStatus::Failed: return "failed";
    case JobStatus::Cancelled: return "cancelled";
  }
  return "unknown";
}

struct RegionBox {
  double x{0.0};
  double y{0.7};
  double width{1.0};
  double height{0.3};

  [[nodiscard]] nlohmann::json to_json() const {
    return {{"x", x}, {"y", y}, {"width", width}, {"height", height}};
  }

  static RegionBox from_json(const nlohmann::json& j) {
    RegionBox box;
    if (j.contains("x") && j["x"].is_number()) box.x = j["x"].get<double>();
    if (j.contains("y") && j["y"].is_number()) box.y = j["y"].get<double>();
    if (j.contains("width") && j["width"].is_number()) box.width = j["width"].get<double>();
    if (j.contains("height") && j["height"].is_number()) box.height = j["height"].get<double>();
    return box;
  }
};

struct JobConfig {
  std::string video_path;
  std::string engine{"paddle"};
  double fps{2.0};
  double confidence_threshold{0.0};
  RegionBox region_box;
};

struct JobEvent {
  std::uint64_t seq_id{0};
  std::string event_type;  // "progress", "push_entry", "done", "error", "cancelled"
  nlohmann::json data;

  [[nodiscard]] std::string to_sse_string(const std::string& job_id) const {
    nlohmann::json payload = data;
    payload["job_id"] = job_id;
    payload["seq"] = seq_id;
    return "id: " + std::to_string(seq_id) + "\nevent: " + event_type + "\ndata: " + payload.dump() + "\n\n";
  }
};

class JobEventStream {
 public:
  void publish(std::string event_type, nlohmann::json data) {
    std::lock_guard<std::mutex> lock(mutex_);
    JobEvent ev{
        .seq_id = ++seq_counter_,
        .event_type = std::move(event_type),
        .data = std::move(data),
    };
    history_.push_back(std::move(ev));
    cv_.notify_all();
  }

  bool fetch_after(std::uint64_t last_seq,
                   std::vector<JobEvent>& out_events,
                   bool is_terminal,
                   std::chrono::milliseconds timeout = std::chrono::milliseconds(2000)) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (history_.empty() || history_.back().seq_id <= last_seq) {
      if (is_terminal) {
        return false;
      }
      cv_.wait_for(lock, timeout, [&] {
        return !history_.empty() && history_.back().seq_id > last_seq;
      });
    }

    out_events.clear();
    for (const auto& ev : history_) {
      if (ev.seq_id > last_seq) {
        out_events.push_back(ev);
      }
    }
    return !out_events.empty();
  }

  [[nodiscard]] std::vector<JobEvent> get_all_history() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {history_.begin(), history_.end()};
  }

 private:
  mutable std::mutex mutex_;
  std::condition_variable cv_;
  std::uint64_t seq_counter_{0};
  std::deque<JobEvent> history_;
};

struct JobContext {
  std::string job_id;
  JobConfig config;

  mutable std::mutex state_mutex;
  JobStatus status{JobStatus::Queued};
  std::atomic<bool> cancel_requested{false};

  std::chrono::system_clock::time_point created_at;
  std::chrono::system_clock::time_point started_at;
  std::chrono::system_clock::time_point ended_at;

  std::string error_message;
  std::vector<sublift::SubtitleEntry> entries;
  std::shared_ptr<JobEventStream> event_stream{std::make_shared<JobEventStream>()};

  [[nodiscard]] bool is_terminal() const {
    std::lock_guard<std::mutex> lock(state_mutex);
    return status == JobStatus::Completed || status == JobStatus::Failed ||
           status == JobStatus::Cancelled;
  }
};

class JobManager {
 public:
  explicit JobManager(std::size_t max_concurrent_jobs = 1, std::size_t max_history_jobs = 50);
  ~JobManager();

  JobManager(const JobManager&) = delete;
  JobManager& operator=(const JobManager&) = delete;
  JobManager(JobManager&&) = delete;
  JobManager& operator=(JobManager&&) = delete;

  [[nodiscard]] std::shared_ptr<JobContext> create_job(const JobConfig& config);
  [[nodiscard]] std::shared_ptr<JobContext> get_job(const std::string& job_id) const;
  bool cancel_job(const std::string& job_id);
  void shutdown();

  [[nodiscard]] static std::string generate_uuid_v4();

 private:
  void worker_loop();
  void execute_job(const std::shared_ptr<JobContext>& job);
  void enforce_lru_cleanup_locked();

  std::size_t max_concurrent_jobs_;
  std::size_t max_history_jobs_;

  mutable std::mutex mutex_;
  std::condition_variable queue_cv_;
  std::atomic<bool> stop_flag_{false};

  std::vector<std::thread> worker_threads_;
  std::deque<std::shared_ptr<JobContext>> pending_queue_;
  std::unordered_map<std::string, std::shared_ptr<JobContext>> jobs_map_;
  std::list<std::string> history_lru_list_;

  // Active bridge handlers for running jobs to allow instant cancellation
  mutable std::mutex bridge_mutex_;
  std::unordered_map<std::string, sublift::worker::BridgeHandler*> active_bridges_;
};

}  // namespace sublift::server
