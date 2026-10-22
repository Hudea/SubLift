#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

#include "sublift/models.hpp"
#include "sublift/ocr_execution.hpp"

namespace sublift::worker {
class BridgeHandler;
}

namespace sublift::server {

enum class JobStatus {
  Queued,
  Preparing,
  Extracting,
  Finalizing,
  Completed,
  Failed,
  Cancelled,
  Interrupted
};

[[nodiscard]] inline std::string to_string(JobStatus status) {
  switch (status) {
    case JobStatus::Queued: return "queued";
    case JobStatus::Preparing: return "preparing";
    case JobStatus::Extracting: return "extracting";
    case JobStatus::Finalizing: return "finalizing";
    case JobStatus::Completed: return "completed";
    case JobStatus::Failed: return "failed";
    case JobStatus::Cancelled: return "cancelled";
    case JobStatus::Interrupted: return "interrupted";
  }
  return "unknown";
}

[[nodiscard]] inline JobStatus job_status_from_string(std::string_view str) {
  if (str == "queued") return JobStatus::Queued;
  if (str == "preparing") return JobStatus::Preparing;
  if (str == "extracting") return JobStatus::Extracting;
  if (str == "finalizing") return JobStatus::Finalizing;
  // Legacy persisted state used a single "running" status; treat it as the
  // extracting stage so old snapshots stay loadable.
  if (str == "running") return JobStatus::Extracting;
  if (str == "completed") return JobStatus::Completed;
  if (str == "failed") return JobStatus::Failed;
  if (str == "cancelled") return JobStatus::Cancelled;
  if (str == "interrupted") return JobStatus::Interrupted;
  return JobStatus::Interrupted;
}

/// contracts/fixtures/statuses.json task -> job mapping. Returns nullopt for
/// task states without a job counterpart (skipped) and unknown inputs.
[[nodiscard]] inline std::optional<std::string_view> task_status_to_job_status(
    std::string_view task) {
  if (task == "waiting") return "queued";
  if (task == "preparing") return "preparing";
  if (task == "extracting") return "extracting";
  if (task == "exporting") return "finalizing";
  if (task == "completed") return "completed";
  if (task == "failed") return "failed";
  if (task == "cancelled") return "cancelled";
  if (task == "interrupted") return "interrupted";
  return std::nullopt;
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

  /// HTTP/UI normalized box -> Worker pixel box, per
  /// contracts/fixtures/roi-cases.json: round each edge, clamp origin to
  /// [0,size-1], clamp width/height to the remaining area with a minimum of 1.
  [[nodiscard]] Box2i to_pixel_box(std::int32_t frame_width,
                                   std::int32_t frame_height) const {
    if (frame_width <= 0 || frame_height <= 0) {
      throw std::invalid_argument("region box requires a non-empty frame");
    }
    const auto clamp01 = [](double value) {
      return std::clamp(value, 0.0, 1.0);
    };
    const auto px_x = std::clamp(
        static_cast<std::int32_t>(std::round(clamp01(x) * frame_width)), 0,
        frame_width - 1);
    const auto px_y = std::clamp(
        static_cast<std::int32_t>(std::round(clamp01(y) * frame_height)), 0,
        frame_height - 1);
    const auto px_w = std::clamp(
        static_cast<std::int32_t>(std::round(clamp01(width) * frame_width)), 1,
        frame_width - px_x);
    const auto px_h = std::clamp(
        static_cast<std::int32_t>(std::round(clamp01(height) * frame_height)), 1,
        frame_height - px_y);
    return Box2i{px_x, px_y, px_w, px_h};
  }
};

struct JobConfig {
  std::string video_path;
  // contracts/fixtures/defaults.json engine_preference[0]; unavailable engines
  // are rejected at the route, never silently replaced.
  std::string engine{"vision"};
  double fps{2.0};
  double confidence_threshold{0.0};
  std::optional<std::string> script;
  RegionBox region_box;
  /// 部署绑定的 Paddle 执行快照（ADR-0041）。仅 paddle 任务携带；旧文件缺省时
  /// 保持未设置，不伪造执行后端。
  std::optional<sublift::PaddleExecutionConfig> paddle_execution;

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
    if (paddle_execution.has_value()) {
      nlohmann::json execution = {
          {"provider", std::string(sublift::paddle_provider_name(paddle_execution->provider))}};
      if (paddle_execution->device_id.has_value()) {
        execution["device_id"] = *paddle_execution->device_id;
      }
      j["paddle_execution"] = std::move(execution);
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
    if (j.contains("paddle_execution") && j["paddle_execution"].is_object()) {
      const auto& execution = j["paddle_execution"];
      if (execution.contains("provider") && execution["provider"].is_string()) {
        if (const auto provider = sublift::parse_paddle_provider(
                execution["provider"].get<std::string>());
            provider.has_value()) {
          sublift::PaddleExecutionConfig snapshot;
          snapshot.provider = *provider;
          const auto& device = execution.value("device_id", nlohmann::json());
          const bool has_device = !device.is_null();
          bool device_ok = true;
          if (has_device) {
            device_ok = false;
            if (device.is_number_integer()) {
              const long long device_value = device.get<long long>();
              device_ok = device_value >= 0 && device_value <= 2147483647;
              if (device_ok) {
                snapshot.device_id = static_cast<std::int32_t>(device_value);
              }
            } else if (device.is_number_unsigned()) {
              const unsigned long long device_value = device.get<unsigned long long>();
              device_ok = device_value <= 2147483647ULL;
              if (device_ok) {
                snapshot.device_id = static_cast<std::int32_t>(device_value);
              }
            }
          }
          // provider 为 CPU 却携带设备，或设备非法：丢弃整个快照，
          // 由 execute_job 回退到部署绑定，不合成部分快照。
          if (!device_ok || (snapshot.provider == sublift::PaddleProvider::Cpu && has_device)) {
            cfg.paddle_execution.reset();
          } else {
            cfg.paddle_execution = snapshot;
          }
        }
      }
    }
    return cfg;
  }
};

struct JobEvent {
  std::uint64_t seq_id{0};
  std::string event_type;  // "progress", "push_entry", "done", "error", "cancelled"
  nlohmann::json data;

  /// Serialize as one SSE frame. Non-const: the event is a consumed copy at
  /// the call site, so `job_id`/`seq` are spliced in place instead of deep-
  /// copying the payload (O5). The dumped bytes are identical to the previous
  /// copy-then-dump form because nlohmann::json objects keep sorted keys.
  [[nodiscard]] std::string to_sse_string(const std::string& job_id) {
    data["job_id"] = job_id;
    data["seq"] = seq_id;
    return "id: " + std::to_string(seq_id) + "\nevent: " + event_type + "\ndata: " + data.dump() + "\n\n";
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
    // O5: bound the replay window. Progress-heavy jobs can publish thousands
    // of events; only the most recent window is retained for cursor resume.
    // Terminal events live at the tail and are never evicted. A cursor older
    // than the window (a reconnect staler than the whole window) resumes
    // from the front - clients dedupe by seq and the final job state remains
    // authoritative via GET /api/jobs/:id (HURDLES 12512).
    while (history_.size() > kMaxHistoryEvents) {
      history_.pop_front();
    }
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
    if (history_.empty() || history_.back().seq_id <= last_seq) {
      return false;  // nothing newer than the cursor (incl. absurd cursors)
    }
    // seq ids are dense (++seq_counter_), so the first event newer than the
    // cursor sits at index (last_seq + 1 - front_seq) - O(k) slice instead of
    // an O(N) head scan (O5).
    const std::uint64_t front_seq = history_.front().seq_id;
    std::size_t begin = 0;
    if (last_seq >= front_seq) {
      const std::uint64_t offset = (last_seq - front_seq) + 1;
      if (offset >= history_.size()) {
        return false;
      }
      begin = static_cast<std::size_t>(offset);
    }
    out_events.reserve(history_.size() - begin);
    for (std::size_t i = begin; i < history_.size(); ++i) {
      out_events.push_back(history_[i]);
    }
    return !out_events.empty();
  }

  [[nodiscard]] std::vector<JobEvent> get_all_history() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return {history_.begin(), history_.end()};
  }

 private:
  /// Replay window per job (O5). 2000 events is far beyond a realistic
  /// reconnect lag (progress is throttled to ~1/s in path mode; frame mode
  /// is throttled by O6) while keeping memory bounded on long jobs.
  static constexpr std::size_t kMaxHistoryEvents = 2000;

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

  /// 设置部署绑定的 Paddle 执行后端（ADR-0041）。提取任务创建引擎时使用。
  void set_paddle_execution(sublift::PaddleExecutionConfig execution);
  [[nodiscard]] sublift::PaddleExecutionConfig paddle_execution() const;

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
  sublift::PaddleExecutionConfig paddle_execution_{};

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

