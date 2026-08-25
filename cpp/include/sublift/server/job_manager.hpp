#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
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
  Cancelled,
  Interrupted
};

[[nodiscard]] inline std::string to_string(JobStatus status) {
  switch (status) {
    case JobStatus::Queued: return "queued";
    case JobStatus::Running: return "running";
    case JobStatus::Completed: return "completed";
    case JobStatus::Failed: return "failed";
    case JobStatus::Cancelled: return "cancelled";
    case JobStatus::Interrupted: return "interrupted";
  }
  return "unknown";
}

[[nodiscard]] inline JobStatus job_status_from_string(std::string_view str) {
  if (str == "queued") return JobStatus::Queued;
  if (str == "running") return JobStatus::Running;
  if (str == "completed") return JobStatus::Completed;
  if (str == "failed") return JobStatus::Failed;
  if (str == "cancelled") return JobStatus::Cancelled;
  if (str == "interrupted") return JobStatus::Interrupted;
  return JobStatus::Interrupted;
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
    // 设计契约 (phase12 §4.2)：region_box 是 [0,1] 归一化比例，入口处夹紧，
    // 拒绝越界/负值传导到像素换算层。
    box.x = std::clamp(box.x, 0.0, 1.0);
    box.y = std::clamp(box.y, 0.0, 1.0);
    box.width = std::clamp(box.width, 0.0, 1.0);
    box.height = std::clamp(box.height, 0.0, 1.0);
    return box;
  }
};

struct JobConfig {
  std::string video_path;
  std::string engine{"paddle"};
  double fps{2.0};
  double confidence_threshold{0.0};
  std::optional<std::string> script;
  RegionBox region_box;

  [[nodiscard]] nlohmann::json to_json() const {
    nlohmann::json j = {
      {"video_path", video_path},
      {"engine", engine},
      {"fps", fps},
      {"confidence_threshold", confidence_threshold},
      {"region_box", region_box.to_json()}
    };
    if (script.has_value()) {
      j["script"] = *script;
    }
    return j;
  }

  static JobConfig from_json(const nlohmann::json& j) {
    JobConfig cfg;
    if (j.contains("video_path") && j["video_path"].is_string()) {
      cfg.video_path = j["video_path"].get<std::string>();
    }
    if (j.contains("engine") && j["engine"].is_string()) {
      cfg.engine = j["engine"].get<std::string>();
    }
    if (j.contains("fps") && j["fps"].is_number()) {
      cfg.fps = j["fps"].get<double>();
    }
    if (j.contains("confidence_threshold") && j["confidence_threshold"].is_number()) {
      cfg.confidence_threshold = j["confidence_threshold"].get<double>();
    }
    if (j.contains("script") && j["script"].is_string()) {
      cfg.script = j["script"].get<std::string>();
    }
    if (j.contains("region_box") && j["region_box"].is_object()) {
      cfg.region_box = RegionBox::from_json(j["region_box"]);
    }
    return cfg;
  }
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
           status == JobStatus::Cancelled || status == JobStatus::Interrupted;
  }

  [[nodiscard]] nlohmann::json to_json() const {
    std::lock_guard<std::mutex> lock(state_mutex);
    auto to_ms = [](const std::chrono::system_clock::time_point& tp) -> std::int64_t {
      return std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()).count();
    };

    nlohmann::json entries_json = nlohmann::json::array();
    for (std::size_t i = 0; i < entries.size(); ++i) {
      const auto& e = entries[i];
      entries_json.push_back({
        {"index", i + 1},
        {"start_ms", e.start_ms},
        {"end_ms", e.end_ms},
        {"text", e.text},
        {"confidence", e.confidence}
      });
    }

    return {
      {"job_id", job_id},
      {"config", config.to_json()},
      {"status", to_string(status)},
      {"created_at_ms", to_ms(created_at)},
      {"started_at_ms", to_ms(started_at)},
      {"ended_at_ms", to_ms(ended_at)},
      {"error_message", error_message},
      {"entries", entries_json}
    };
  }
};

class JobManager {
 public:
  explicit JobManager(std::size_t max_concurrent_jobs = 1,
                      std::size_t max_history_jobs = 50,
                      std::filesystem::path state_file_path = {});
  ~JobManager();

  JobManager(const JobManager&) = delete;
  JobManager& operator=(const JobManager&) = delete;
  JobManager(JobManager&&) = delete;
  JobManager& operator=(JobManager&&) = delete;

  [[nodiscard]] std::shared_ptr<JobContext> create_job(const JobConfig& config);
  [[nodiscard]] std::shared_ptr<JobContext> create_completed_job(const JobConfig& config,
                                                                 std::vector<sublift::SubtitleEntry> entries = {});
  [[nodiscard]] std::shared_ptr<JobContext> get_job(const std::string& job_id) const;
  [[nodiscard]] std::vector<std::shared_ptr<JobContext>> get_all_jobs() const;
  bool cancel_job(const std::string& job_id);
  void shutdown();

  void set_state_file_path(const std::filesystem::path& path);
  [[nodiscard]] std::filesystem::path get_state_file_path() const;

  void save_state();
  void load_state();

  [[nodiscard]] static std::string generate_uuid_v4();

 private:
  void worker_loop();
  void execute_job(const std::shared_ptr<JobContext>& job);
  void enforce_lru_cleanup_locked();
  void save_state_locked();
  void load_state_locked();

  std::size_t max_concurrent_jobs_;
  std::size_t max_history_jobs_;
  std::filesystem::path state_file_path_;

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

