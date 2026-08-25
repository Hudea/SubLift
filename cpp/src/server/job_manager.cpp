#include "sublift/server/job_manager.hpp"

#include <cmath>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <random>
#include <sstream>

#if defined(_POSIX_C_SOURCE) || defined(__APPLE__) || defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#endif

#include "bridge.hpp"
#include "detector_factory.hpp"
#include "engine_factory.hpp"
#include "path_media_services.hpp"
#include "sublift/adapters/ffmpeg.hpp"
#include "sublift/protocol/protocol.hpp"

namespace sublift::server {

std::string JobManager::generate_uuid_v4() {
  static thread_local std::mt19937_64 gen(std::random_device{}());
  static thread_local std::uniform_int_distribution<std::uint64_t> dist;

  std::uint64_t part1 = dist(gen);
  std::uint64_t part2 = dist(gen);

  // Set UUID version to 4 (0100) and variant to 10
  part1 = (part1 & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
  part2 = (part2 & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;

  std::ostringstream oss;
  oss << std::hex << std::setfill('0')
      << std::setw(8) << ((part1 >> 32) & 0xFFFFFFFFULL) << "-"
      << std::setw(4) << ((part1 >> 16) & 0xFFFFULL) << "-"
      << std::setw(4) << (part1 & 0xFFFFULL) << "-"
      << std::setw(4) << ((part2 >> 48) & 0xFFFFULL) << "-"
      << std::setw(12) << (part2 & 0xFFFFFFFFFFFFULL);
  return oss.str();
}

JobManager::JobManager(std::size_t max_concurrent_jobs,
                       std::size_t max_history_jobs,
                       std::filesystem::path state_file_path)
    : max_concurrent_jobs_(std::max<std::size_t>(1, max_concurrent_jobs)),
      max_history_jobs_(std::max<std::size_t>(10, max_history_jobs)),
      state_file_path_(std::move(state_file_path)) {
  if (!state_file_path_.empty()) {
    std::lock_guard<std::mutex> lock(mutex_);
    load_state_locked();
  }

  worker_threads_.reserve(max_concurrent_jobs_);
  for (std::size_t i = 0; i < max_concurrent_jobs_; ++i) {
    worker_threads_.emplace_back(&JobManager::worker_loop, this);
  }
}

JobManager::~JobManager() {
  shutdown();
}

void JobManager::shutdown() {
  stop_flag_.store(true, std::memory_order_release);
  queue_cv_.notify_all();

  // Cancel all active bridges to unblock running threads
  {
    std::lock_guard<std::mutex> lk(bridge_mutex_);
    for (auto& [id, bridge] : active_bridges_) {
      if (bridge) {
        bridge->cancel_job();
      }
    }
  }

  for (auto& th : worker_threads_) {
    if (th.joinable()) {
      th.join();
    }
  }
  worker_threads_.clear();

  {
    std::lock_guard<std::mutex> lock(mutex_);
    save_state_locked();
  }
}

std::shared_ptr<JobContext> JobManager::create_job(const JobConfig& config) {
  auto job = std::make_shared<JobContext>();
  job->job_id = generate_uuid_v4();
  job->config = config;
  job->created_at = std::chrono::system_clock::now();
  job->status = JobStatus::Queued;

  {
    std::lock_guard<std::mutex> lock(mutex_);
    jobs_map_[job->job_id] = job;
    history_lru_list_.push_front(job->job_id);
    pending_queue_.push_back(job);
    enforce_lru_cleanup_locked();
    save_state_locked();
  }

  queue_cv_.notify_one();
  return job;
}

std::shared_ptr<JobContext> JobManager::create_completed_job(const JobConfig& config,
                                                             std::vector<sublift::SubtitleEntry> entries) {
  auto job = std::make_shared<JobContext>();
  job->job_id = generate_uuid_v4();
  job->config = config;
  job->created_at = std::chrono::system_clock::now();
  job->started_at = job->created_at;
  job->ended_at = job->created_at;
  job->status = JobStatus::Completed;
  job->entries = std::move(entries);

  {
    std::lock_guard<std::mutex> lock(mutex_);
    jobs_map_[job->job_id] = job;
    history_lru_list_.push_front(job->job_id);
    enforce_lru_cleanup_locked();
    save_state_locked();
  }

  return job;
}

std::shared_ptr<JobContext> JobManager::get_job(const std::string& job_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = jobs_map_.find(job_id);
  if (it != jobs_map_.end()) {
    return it->second;
  }
  return nullptr;
}

std::vector<std::shared_ptr<JobContext>> JobManager::get_all_jobs() const {
  std::lock_guard<std::mutex> lock(mutex_);
  std::vector<std::shared_ptr<JobContext>> result;
  result.reserve(history_lru_list_.size());
  for (const auto& id : history_lru_list_) {
    auto it = jobs_map_.find(id);
    if (it != jobs_map_.end()) {
      result.push_back(it->second);
    }
  }
  return result;
}

bool JobManager::cancel_job(const std::string& job_id) {
  std::shared_ptr<JobContext> job;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = jobs_map_.find(job_id);
    if (it == jobs_map_.end()) return false;
    job = it->second;
  }

  job->cancel_requested.store(true, std::memory_order_release);

  {
    std::lock_guard<std::mutex> lk(bridge_mutex_);
    auto bit = active_bridges_.find(job_id);
    if (bit != active_bridges_.end() && bit->second) {
      bit->second->cancel_job();
    }
  }

  {
    std::lock_guard<std::mutex> job_lock(job->state_mutex);
    if (job->status == JobStatus::Queued) {
      job->status = JobStatus::Cancelled;
      job->ended_at = std::chrono::system_clock::now();
      job->event_stream->publish("cancelled", {{"reason", "Cancelled while queued"}});
    }
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    save_state_locked();
  }

  return true;
}

void JobManager::set_state_file_path(const std::filesystem::path& path) {
  std::lock_guard<std::mutex> lock(mutex_);
  state_file_path_ = path;
  if (!state_file_path_.empty()) {
    load_state_locked();
  }
}

std::filesystem::path JobManager::get_state_file_path() const {
  std::lock_guard<std::mutex> lock(mutex_);
  return state_file_path_;
}

void JobManager::save_state() {
  std::lock_guard<std::mutex> lock(mutex_);
  save_state_locked();
}

void JobManager::load_state() {
  std::lock_guard<std::mutex> lock(mutex_);
  load_state_locked();
}

void JobManager::save_state_locked() {
  if (state_file_path_.empty()) return;

  std::error_code ec;
  auto parent_dir = state_file_path_.parent_path();
  if (!parent_dir.empty()) {
    std::filesystem::create_directories(parent_dir, ec);
  }

  nlohmann::json root;
  root["version"] = 1;
  root["updated_at_ms"] = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();

  nlohmann::json jobs_array = nlohmann::json::array();
  for (const auto& job_id : history_lru_list_) {
    auto it = jobs_map_.find(job_id);
    if (it != jobs_map_.end()) {
      jobs_array.push_back(it->second->to_json());
    }
  }
  root["jobs"] = jobs_array;

  auto tmp_path = (parent_dir.empty() ? std::filesystem::path(".") : parent_dir) /
                  ("." + state_file_path_.filename().string() + ".tmp." + generate_uuid_v4());

  {
    std::ofstream ofs(tmp_path, std::ios::out | std::ios::trunc);
    if (!ofs) return;
    ofs << root.dump(2) << "\n";
    ofs.flush();
  }

#if defined(_POSIX_C_SOURCE) || defined(__APPLE__) || defined(__linux__)
  int fd = ::open(tmp_path.c_str(), O_RDWR);
  if (fd >= 0) {
#if defined(__APPLE__) && defined(F_FULLFSYNC)
    ::fcntl(fd, F_FULLFSYNC);
#else
    ::fsync(fd);
#endif
    ::close(fd);
  }
#endif

  std::filesystem::rename(tmp_path, state_file_path_, ec);
  if (ec) {
    std::filesystem::remove(tmp_path, ec);
  }
}

void JobManager::load_state_locked() {
  if (state_file_path_.empty()) return;

  std::error_code ec;
  if (!std::filesystem::exists(state_file_path_, ec) ||
      !std::filesystem::is_regular_file(state_file_path_, ec)) {
    return;
  }

  std::ifstream ifs(state_file_path_);
  if (!ifs) return;

  nlohmann::json root;
  try {
    ifs >> root;
  } catch (...) {
    // Corrupted state file: gracefully ignore
    return;
  }

  if (!root.is_object() || !root.contains("version") || !root.contains("jobs") || !root["jobs"].is_array()) {
    return;
  }

  int version = 0;
  if (root["version"].is_number_integer()) {
    version = root["version"].get<int>();
  }
  if (version != 1) {
    return;
  }

  auto from_ms = [](std::int64_t ms) -> std::chrono::system_clock::time_point {
    return std::chrono::system_clock::time_point(std::chrono::milliseconds(ms));
  };

  bool state_changed = false;

  for (const auto& job_json : root["jobs"]) {
    if (!job_json.is_object() || !job_json.contains("job_id") || !job_json["job_id"].is_string()) {
      continue;
    }

    std::string job_id = job_json["job_id"].get<std::string>();
    if (job_id.empty() || jobs_map_.find(job_id) != jobs_map_.end()) {
      continue;
    }

    auto job = std::make_shared<JobContext>();
    job->job_id = job_id;

    if (job_json.contains("config") && job_json["config"].is_object()) {
      job->config = JobConfig::from_json(job_json["config"]);
    }

    std::string status_str = "interrupted";
    if (job_json.contains("status") && job_json["status"].is_string()) {
      status_str = job_json["status"].get<std::string>();
    }

    if (status_str == "queued" || status_str == "running") {
      // Incomplete jobs MUST transition to Interrupted and remain paused!
      job->status = JobStatus::Interrupted;
      state_changed = true;
    } else {
      job->status = job_status_from_string(status_str);
    }

    if (job_json.contains("created_at_ms") && job_json["created_at_ms"].is_number_integer()) {
      job->created_at = from_ms(job_json["created_at_ms"].get<std::int64_t>());
    }
    if (job_json.contains("started_at_ms") && job_json["started_at_ms"].is_number_integer()) {
      job->started_at = from_ms(job_json["started_at_ms"].get<std::int64_t>());
    }
    if (job_json.contains("ended_at_ms") && job_json["ended_at_ms"].is_number_integer()) {
      job->ended_at = from_ms(job_json["ended_at_ms"].get<std::int64_t>());
    }
    if (job_json.contains("error_message") && job_json["error_message"].is_string()) {
      job->error_message = job_json["error_message"].get<std::string>();
    }

    if (job_json.contains("entries") && job_json["entries"].is_array()) {
      for (const auto& ej : job_json["entries"]) {
        if (!ej.is_object()) continue;
        sublift::SubtitleEntry entry;
        if (ej.contains("start_ms") && ej["start_ms"].is_number_integer()) {
          entry.start_ms = ej["start_ms"].get<std::int64_t>();
        }
        if (ej.contains("end_ms") && ej["end_ms"].is_number_integer()) {
          entry.end_ms = ej["end_ms"].get<std::int64_t>();
        }
        if (ej.contains("text") && ej["text"].is_string()) {
          entry.text = ej["text"].get<std::string>();
        }
        if (ej.contains("confidence") && ej["confidence"].is_number()) {
          entry.confidence = ej["confidence"].get<double>();
        }
        job->entries.push_back(std::move(entry));
      }
    }

    // Populate terminal event in job's event stream
    if (job->status == JobStatus::Completed) {
      auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(job->ended_at - job->started_at).count();
      job->event_stream->publish("done", {
          {"ok", true},
          {"total_entries", job->entries.size()},
          {"elapsed_ms", std::max<std::int64_t>(0, elapsed_ms)}
      });
    } else if (job->status == JobStatus::Interrupted) {
      job->event_stream->publish("error", {
          {"error", "Job was interrupted by server shutdown"}
      });
    } else if (job->status == JobStatus::Failed) {
      job->event_stream->publish("error", {
          {"error", job->error_message.empty() ? "Job failed" : job->error_message}
      });
    } else if (job->status == JobStatus::Cancelled) {
      job->event_stream->publish("cancelled", {
          {"cancelled", true},
          {"reason", "Cancelled"}
      });
    }

    jobs_map_[job->job_id] = job;
    history_lru_list_.push_back(job->job_id);
    // NOTICE: Do NOT add to pending_queue_!
  }

  enforce_lru_cleanup_locked();

  if (state_changed) {
    save_state_locked();
  }
}

void JobManager::enforce_lru_cleanup_locked() {
  while (jobs_map_.size() > max_history_jobs_) {
    bool removed = false;
    for (auto it = history_lru_list_.rbegin(); it != history_lru_list_.rend(); ++it) {
      const std::string& old_id = *it;
      auto job_it = jobs_map_.find(old_id);
      if (job_it != jobs_map_.end() && job_it->second->is_terminal()) {
        jobs_map_.erase(job_it);
        history_lru_list_.erase(std::next(it).base());
        removed = true;
        break;
      }
    }
    if (!removed) break;
  }
}

void JobManager::worker_loop() {
  while (!stop_flag_.load(std::memory_order_acquire)) {
    std::shared_ptr<JobContext> job;
    {
      std::unique_lock<std::mutex> lock(mutex_);
      queue_cv_.wait(lock, [&] {
        return stop_flag_.load(std::memory_order_acquire) || !pending_queue_.empty();
      });

      if (stop_flag_.load(std::memory_order_acquire)) return;

      job = pending_queue_.front();
      pending_queue_.pop_front();
    }

    if (!job) continue;

    if (job->cancel_requested.load(std::memory_order_acquire)) {
      std::lock_guard<std::mutex> job_lock(job->state_mutex);
      job->status = JobStatus::Cancelled;
      job->ended_at = std::chrono::system_clock::now();
      job->event_stream->publish("cancelled", {{"reason", "Cancelled prior to run"}});
      {
        std::lock_guard<std::mutex> lock(mutex_);
        save_state_locked();
      }
      continue;
    }

    execute_job(job);
  }
}

void JobManager::execute_job(const std::shared_ptr<JobContext>& job) {
  auto start_time = std::chrono::steady_clock::now();

  {
    std::lock_guard<std::mutex> job_lock(job->state_mutex);
    job->status = JobStatus::Running;
    job->started_at = std::chrono::system_clock::now();
  }
  {
    std::lock_guard<std::mutex> lock(mutex_);
    save_state_locked();
  }

  std::mutex done_mutex;
  std::condition_variable done_cv;
  bool finished = false;
  bool is_error = false;
  std::string error_msg;

  try {
    auto engine_factory = std::make_unique<sublift::worker::EngineFactory>(job->config.engine);
    auto media_services = std::make_unique<sublift::worker::FfmpegPathMediaServices>();
    auto detector_factory = std::make_unique<sublift::worker::DefaultDetectorFactory>();

    auto bridge = std::make_unique<sublift::worker::BridgeHandler>(
        std::move(engine_factory), std::move(media_services), std::move(detector_factory));

    {
      std::lock_guard<std::mutex> lk(bridge_mutex_);
      active_bridges_[job->job_id] = bridge.get();
    }

    sublift::worker::PushCallback push_cb = [&](const sublift::ipc::Message& msg) {
      if (std::holds_alternative<sublift::ipc::ProgressMsg>(msg)) {
        const auto& p = std::get<sublift::ipc::ProgressMsg>(msg);
        job->event_stream->publish("progress", {
            {"stage", p.stage},
            {"pct", p.pct},
            {"eta_ms", p.eta_ms},
            {"fps", job->config.fps}
        });
      } else if (std::holds_alternative<sublift::ipc::PushEntryMsg>(msg)) {
        const auto& pe = std::get<sublift::ipc::PushEntryMsg>(msg);
        std::size_t idx = 0;
        {
          std::lock_guard<std::mutex> job_lock(job->state_mutex);
          job->entries.push_back(pe.entry);
          idx = job->entries.size();
        }
        job->event_stream->publish("push_entry", {
            {"entry", {
                {"index", idx},
                {"start_ms", pe.entry.start_ms},
                {"end_ms", pe.entry.end_ms},
                {"text", pe.entry.text},
                {"confidence", pe.entry.confidence}
            }}
        });
      } else if (std::holds_alternative<sublift::ipc::EntriesMsg>(msg)) {
        const auto& entries_msg = std::get<sublift::ipc::EntriesMsg>(msg);
        std::lock_guard<std::mutex> job_lock(job->state_mutex);
        job->entries = entries_msg.entries;
      } else if (std::holds_alternative<sublift::ipc::DoneMsg>(msg)) {
        const auto& done_msg = std::get<sublift::ipc::DoneMsg>(msg);
        std::lock_guard<std::mutex> lk(done_mutex);
        finished = true;
        if (!done_msg.ok) {
          is_error = true;
          error_msg = done_msg.error.value_or("Pipeline processing failed");
        }
        done_cv.notify_one();
      } else if (std::holds_alternative<sublift::ipc::ErrorMsg>(msg)) {
        const auto& err_msg = std::get<sublift::ipc::ErrorMsg>(msg);
        std::lock_guard<std::mutex> lk(done_mutex);
        finished = true;
        is_error = true;
        error_msg = err_msg.message;
        done_cv.notify_one();
      }
    };

    sublift::ipc::StartJobMsg start_msg;
    start_msg.video_id = job->job_id;
    start_msg.video_path = job->config.video_path;
    start_msg.engine = job->config.engine;
    start_msg.fps = job->config.fps;
    start_msg.confidence_threshold = job->config.confidence_threshold;

    if (job->config.region_box.width > 0.0 && job->config.region_box.height > 0.0) {
      try {
        auto source_info = sublift::ffmpeg::probe_source_frame(job->config.video_path);
        std::int32_t px_x = static_cast<std::int32_t>(std::round(job->config.region_box.x * source_info.width));
        std::int32_t px_y = static_cast<std::int32_t>(std::round(job->config.region_box.y * source_info.height));
        std::int32_t px_w = static_cast<std::int32_t>(std::round(job->config.region_box.width * source_info.width));
        std::int32_t px_h = static_cast<std::int32_t>(std::round(job->config.region_box.height * source_info.height));

        // 先把原点夹到 [0, size-1]，保证随后 w/h clamp 的上界 >= 下界 1；
        // 否则 x==width 时 std::clamp(v, 1, 0) 属于未定义行为。
        px_x = std::clamp(px_x, 0, source_info.width - 1);
        px_y = std::clamp(px_y, 0, source_info.height - 1);
        px_w = std::clamp(px_w, 1, source_info.width - px_x);
        px_h = std::clamp(px_h, 1, source_info.height - px_y);

        start_msg.region_box = sublift::Box2i{px_x, px_y, px_w, px_h};
      } catch (...) {
        // Fall back without region_box
      }
    }

    // Start job in BridgeHandler
    auto sync_res = bridge->handle(start_msg, push_cb);
    if (sync_res.has_value()) {
      if (std::holds_alternative<sublift::ipc::DoneMsg>(*sync_res)) {
        const auto& done_msg = std::get<sublift::ipc::DoneMsg>(*sync_res);
        if (!done_msg.ok) {
          is_error = true;
          error_msg = done_msg.error.value_or("Job start returned failure");
          finished = true;
        }
      } else if (std::holds_alternative<sublift::ipc::ErrorMsg>(*sync_res)) {
        const auto& err_msg = std::get<sublift::ipc::ErrorMsg>(*sync_res);
        is_error = true;
        error_msg = err_msg.message;
        finished = true;
      }
    }

    // Wait for completion, cancellation, or error
    {
      std::unique_lock<std::mutex> lk(done_mutex);
      done_cv.wait(lk, [&] {
        return finished || job->cancel_requested.load(std::memory_order_acquire) ||
               stop_flag_.load(std::memory_order_acquire);
      });
    }

    if (job->cancel_requested.load(std::memory_order_acquire)) {
      bridge->cancel_job();
    }

    // Remove from active_bridges_ BEFORE destroying bridge to prevent UAF in cancel_job
    {
      std::lock_guard<std::mutex> lk(bridge_mutex_);
      active_bridges_.erase(job->job_id);
    }

    // BridgeHandler destructor safely waits for its background worker thread to finish
    bridge.reset();

    auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start_time).count();

    std::lock_guard<std::mutex> job_lock(job->state_mutex);
    job->ended_at = std::chrono::system_clock::now();

    if (job->cancel_requested.load(std::memory_order_acquire)) {
      job->status = JobStatus::Cancelled;
      job->event_stream->publish("cancelled", {
          {"cancelled", true},
          {"reason", "User requested cancellation"}
      });
    } else if (is_error) {
      job->status = JobStatus::Failed;
      job->error_message = error_msg;
      job->event_stream->publish("error", {{"error", error_msg}});
    } else {
      job->status = JobStatus::Completed;
      job->event_stream->publish("done", {
          {"ok", true},
          {"total_entries", job->entries.size()},
          {"elapsed_ms", elapsed_ms}
      });
    }

  } catch (const std::exception& e) {
    {
      std::lock_guard<std::mutex> lk(bridge_mutex_);
      active_bridges_.erase(job->job_id);
    }
    std::lock_guard<std::mutex> job_lock(job->state_mutex);
    job->status = JobStatus::Failed;
    job->error_message = e.what();
    job->ended_at = std::chrono::system_clock::now();
    job->event_stream->publish("error", {{"error", e.what()}});
  }

  {
    std::lock_guard<std::mutex> lock(mutex_);
    save_state_locked();
  }
}

}  // namespace sublift::server
