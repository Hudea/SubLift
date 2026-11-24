#include "worker_app.hpp"

#include "image_io.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <sublift/adapters/apple_vision/apple_vision_ocr.hpp>
#include <sublift/adapters/ffmpeg/ffmpeg_frame_source.hpp>
#include <sublift/adapters/paddle/paddle_ocr.hpp>
#include <sublift/core/box.hpp>
#include <sublift/core/models.hpp>
#include <sublift/core/subtitle.hpp>
#include <sublift/engine/auto_region.hpp>
#include <sublift/engine/config.hpp>
#include <sublift/engine/observer.hpp>
#include <sublift/engine/session.hpp>
#include <sublift/text/cleaning.hpp>
#include <sublift/text/dedupe.hpp>
#include <sublift/text/line_select.hpp>
#include <sublift/text/srt.hpp>

namespace sublift::worker::detail {

[[nodiscard]] std::string to_lower_ascii(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (unsigned char c : s) {
    if (c >= 'A' && c <= 'Z') {
      out.push_back(static_cast<char>(c - 'A' + 'a'));
    } else {
      out.push_back(static_cast<char>(c));
    }
  }
  return out;
}

[[nodiscard]] std::string platform_name() {
#if defined(__APPLE__)
  return "macos";
#elif defined(_WIN32)
  return "windows";
#else
  return "linux";
#endif
}

[[nodiscard]] std::vector<std::string> platform_hwaccel_modes() {
#if defined(__APPLE__)
  return {"auto", "none", "videotoolbox"};
#elif defined(_WIN32)
  return {"auto", "none", "cuda", "qsv", "d3d11va", "dxva2", "vulkan"};
#else
  return {"auto", "none", "cuda", "vaapi", "qsv", "vulkan"};
#endif
}

[[nodiscard]] nlohmann::json source_box_to_json(const SourceBox& box) {
  return nlohmann::json{
      {"x", box.x},
      {"y", box.y},
      {"width", box.width},
      {"height", box.height},
  };
}

[[nodiscard]] bool parse_source_box(const nlohmann::json& j,
                                    SourceBox& out,
                                    std::string& err) {
  if (!j.is_object()) {
    err = "crop/region_box must be a JSON object";
    return false;
  }
  for (const char* key : {"x", "y", "width", "height"}) {
    if (!j.contains(key) || !j[key].is_number_integer()) {
      err = std::string("Missing or non-integer field '") + key + "' in box";
      return false;
    }
  }
  out.x = j["x"].get<std::int32_t>();
  out.y = j["y"].get<std::int32_t>();
  out.width = j["width"].get<std::int32_t>();
  out.height = j["height"].get<std::int32_t>();
  if (out.x < 0 || out.y < 0 || out.width <= 0 || out.height <= 0) {
    err = "Box requires x >= 0, y >= 0, width > 0, height > 0";
    return false;
  }
  return true;
}

[[nodiscard]] nlohmann::json subtitle_entry_to_json(const SubtitleEntry& e) {
  return nlohmann::json{
      {"id", e.id},
      {"start_ms", e.range.start_ms},
      {"end_ms", e.range.end_ms},
      {"text", e.text},
      {"confidence", e.confidence},
  };
}

[[nodiscard]] bool parse_subtitle_entries(const nlohmann::json& arr,
                                          std::vector<SubtitleEntry>& out,
                                          std::string& err) {
  if (!arr.is_array()) {
    err = "'entries' must be an array";
    return false;
  }
  out.clear();
  out.reserve(arr.size());
  std::uint64_t fallback_id = 1;
  for (const auto& item : arr) {
    if (!item.is_object()) {
      err = "Each subtitle entry must be an object";
      return false;
    }
    if (!item.contains("start_ms") || !item["start_ms"].is_number_integer() ||
        !item.contains("end_ms") || !item["end_ms"].is_number_integer()) {
      err = "Subtitle entry requires integer 'start_ms' and 'end_ms'";
      return false;
    }
    if (!item.contains("text") || !item["text"].is_string()) {
      err = "Subtitle entry requires string 'text'";
      return false;
    }
    SubtitleEntry e;
    e.id = item.contains("id") && item["id"].is_number_unsigned()
        ? item["id"].get<std::uint64_t>()
        : fallback_id;
    ++fallback_id;
    e.range.start_ms = item["start_ms"].get<std::int64_t>();
    e.range.end_ms = item["end_ms"].get<std::int64_t>();
    if (e.range.end_ms <= e.range.start_ms || e.range.end_ms <= 0) {
      err = "Subtitle entry requires end_ms > start_ms and end_ms > 0";
      return false;
    }
    e.text = item["text"].get<std::string>();
    e.confidence = item.contains("confidence") && item["confidence"].is_number()
        ? item["confidence"].get<double>()
        : 1.0;
    out.push_back(std::move(e));
  }
  return true;
}

[[nodiscard]] nlohmann::json metrics_to_json(const ExtractionMetrics& m) {
  return nlohmann::json{
      {"total_frames_processed", m.total_frames_processed},
      {"total_segments_detected", m.total_segments_detected},
      {"total_ocr_calls", m.total_ocr_calls},
      {"total_entries_produced", m.total_entries_produced},
      {"wall_time_ms", m.wall_time_ms},
      {"effective_fps", m.effective_fps},
  };
}

[[nodiscard]] nlohmann::json profile_to_json(const SubtitleProfile& p) {
  return nlohmann::json{
      {"script", p.script},
      {"center_x", p.center_x},
      {"center_y", p.center_y},
      {"height", p.height},
      {"y_min", p.y_min},
      {"y_max", p.y_max},
  };
}

[[nodiscard]] std::string error_code_to_string(ExtractionErrorCode code) {
  switch (code) {
    case ExtractionErrorCode::SourceReadError:
      return "SourceReadError";
    case ExtractionErrorCode::DecoderFailed:
      return "DecoderFailed";
    case ExtractionErrorCode::OcrFailed:
      return "OcrFailed";
    case ExtractionErrorCode::InvalidConfiguration:
      return "InvalidConfiguration";
    case ExtractionErrorCode::Aborted:
      return "Aborted";
  }
  return "InternalError";
}

class NullOcrEngine final : public IOcrEngine {
 public:
  [[nodiscard]] OcrResult recognize(const ImageView& /*image*/) override {
    return OcrResult{};
  }
  [[nodiscard]] std::string engine_name() const override {
    return "null";
  }
};

}  // namespace sublift::worker::detail

namespace sublift::worker {

OcrResult LockedOcrEngineProxy::recognize(const ImageView& image) {
  if (!inner_ || !mutex_) {
    return OcrResult{};
  }
  std::lock_guard<std::mutex> lock(*mutex_);
  return inner_->recognize(image);
}

std::string LockedOcrEngineProxy::engine_name() const {
  return inner_ ? inner_->engine_name() : "";
}

DefaultWorkerBackend::DefaultWorkerBackend()
    : paddle_mutex_(std::make_shared<std::mutex>()),
      vision_mutex_(std::make_shared<std::mutex>()) {}

DefaultWorkerBackend::~DefaultWorkerBackend() = default;

CapabilitiesInfo DefaultWorkerBackend::query_capabilities() {
  CapabilitiesInfo info;
  if (sublift::adapters::paddle::is_paddle_available()) {
    info.engines.emplace_back("paddle");
  }
#if defined(SUBLIFT_HAS_APPLE_VISION) && SUBLIFT_HAS_APPLE_VISION
  if (sublift::adapters::apple_vision::is_vision_available()) {
    info.engines.emplace_back("apple_vision");
  }
#endif

  try {
    (void)sublift::adapters::ffmpeg::resolve_ffmpeg_bin();
    info.ffmpeg_available = true;
  } catch (...) {
    info.ffmpeg_available = false;
  }

  try {
    (void)sublift::adapters::ffmpeg::resolve_ffprobe_bin();
    info.ffprobe_available = true;
  } catch (...) {
    info.ffprobe_available = false;
  }

  info.hwaccel_modes = detail::platform_hwaccel_modes();
  info.platform = detail::platform_name();
  return info;
}

std::unique_ptr<IFrameSource> DefaultWorkerBackend::create_frame_source(
    double fps,
    const std::optional<std::string>& hwaccel,
    std::int32_t max_output_width) {
  sublift::adapters::ffmpeg::FfmpegExtractorOptions opts;
  opts.max_output_width = max_output_width;
  return std::make_unique<sublift::adapters::ffmpeg::FfmpegFrameSource>(
      fps, std::nullopt, std::nullopt, hwaccel, opts);
}

OcrAcquireResult DefaultWorkerBackend::acquire_ocr_engine(
    const std::string& engine_id,
    std::int32_t reference_width) {
  if (engine_id == "paddle") {
    if (!sublift::adapters::paddle::is_paddle_available()) {
      return OcrAcquireResult{
          nullptr,
          "EngineUnavailable",
          "PaddleOCR model bundle or runtime is unavailable",
      };
    }
    std::lock_guard<std::mutex> lock(engines_mutex_);
    if (!paddle_engine_) {
      try {
        sublift::adapters::paddle::PaddleOptions opts;
        opts.model_type = "small";
        opts.intra_op_threads = 4;
        opts.det_geometry.pixel_budget =
            sublift::adapters::paddle::kProductDetPixelBudget;
        opts.det_geometry.reference_width =
            reference_width > 0
                ? std::min(2000.0, static_cast<double>(reference_width))
                : 1920.0;
        paddle_engine_ =
            std::make_shared<sublift::adapters::paddle::PaddleOcrEngine>(opts);
      } catch (const std::exception& ex) {
        return OcrAcquireResult{
            nullptr,
            "EngineUnavailable",
            std::string("Failed to initialize PaddleOCR: ") + ex.what(),
        };
      }
    }
    return OcrAcquireResult{
        std::make_shared<LockedOcrEngineProxy>(paddle_engine_, paddle_mutex_),
        "",
        "",
    };
  }

  if (engine_id == "apple_vision") {
#if defined(SUBLIFT_HAS_APPLE_VISION) && SUBLIFT_HAS_APPLE_VISION
    if (!sublift::adapters::apple_vision::is_vision_available()) {
      return OcrAcquireResult{
          nullptr,
          "EngineUnavailable",
          "Apple Vision framework is unavailable at runtime",
      };
    }
    std::lock_guard<std::mutex> lock(engines_mutex_);
    if (!vision_engine_) {
      try {
        vision_engine_ =
            std::make_shared<sublift::adapters::apple_vision::AppleVisionOcrEngine>();
      } catch (const std::exception& ex) {
        return OcrAcquireResult{
            nullptr,
            "EngineUnavailable",
            std::string("Failed to initialize Apple Vision: ") + ex.what(),
        };
      }
    }
    return OcrAcquireResult{
        std::make_shared<LockedOcrEngineProxy>(vision_engine_, vision_mutex_),
        "",
        "",
    };
#else
    return OcrAcquireResult{
        nullptr,
        "EngineUnavailable",
        "Apple Vision OCR engine is disabled at build time (SUBLIFT_ENABLE_VISION=OFF)",
    };
#endif
  }

  return OcrAcquireResult{
      nullptr,
      "InvalidConfiguration",
      "Unknown OCR engine: '" + engine_id + "'",
  };
}

}  // namespace sublift::worker

namespace sublift::worker::detail {

struct TaskState {
  std::uint64_t id{0};
  std::atomic<bool> cancelled{false};
  std::atomic<bool> finished{false};
  std::mutex hook_mutex;
  IFrameSource* active_source{nullptr};
  ExtractionSession* active_session{nullptr};
  std::thread thread;

  void bind_source(IFrameSource* src) {
    std::lock_guard<std::mutex> lock(hook_mutex);
    active_source = src;
    if (cancelled.load(std::memory_order_acquire) && active_source) {
      active_source->cancel();
    }
  }

  void bind_session(ExtractionSession* sess) {
    std::lock_guard<std::mutex> lock(hook_mutex);
    active_session = sess;
    if (cancelled.load(std::memory_order_acquire) && active_session) {
      active_session->cancel();
    }
  }

  void unbind_all() {
    std::lock_guard<std::mutex> lock(hook_mutex);
    active_session = nullptr;
    active_source = nullptr;
  }

  void request_cancel() {
    cancelled.store(true, std::memory_order_release);
    std::lock_guard<std::mutex> lock(hook_mutex);
    if (active_session) {
      active_session->cancel();
    }
    if (active_source) {
      active_source->cancel();
    }
  }
};

class WorkerExtractionObserver final : public IExtractionObserver {
 public:
  WorkerExtractionObserver(
      std::uint64_t task_id,
      bool pure_timing_strips,
      std::filesystem::path strips_dir,
      std::function<void(const std::string&, const nlohmann::json&)> emit_fn)
      : task_id_(task_id),
        pure_timing_strips_(pure_timing_strips),
        strips_dir_(std::move(strips_dir)),
        emit_fn_(std::move(emit_fn)) {}

  void on_progress(std::int64_t current_ms, std::int64_t total_ms, double fps) override {
    const auto now = std::chrono::steady_clock::now();
    ++progress_frames_;
    if (progress_frames_ == 1) {
      first_progress_tp_ = now;
    }
    const bool at_end = (total_ms > 0 && current_ms >= total_ms);
    if (has_emitted_progress_ && !at_end) {
      const auto elapsed_ms =
          std::chrono::duration_cast<std::chrono::milliseconds>(now - last_progress_tp_)
              .count();
      if (elapsed_ms < 50) {
        return;
      }
    }
    has_emitted_progress_ = true;
    last_progress_tp_ = now;
    const auto total_elapsed_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now - first_progress_tp_)
            .count();
    const double real_fps =
        (total_elapsed_ms > 0)
            ? (static_cast<double>(progress_frames_) * 1000.0 /
               static_cast<double>(total_elapsed_ms))
            : fps;
    emit_fn_("progress", nlohmann::json{
                             {"current_ms", current_ms},
                             {"total_ms", total_ms},
                             {"fps", real_fps},
                         });
  }

  void on_segment_detected(const timing::Segment& seg) override {
    if (!pure_timing_strips_ || strips_dir_.empty()) {
      return;
    }
    if (seg.end_ms <= seg.start_ms || seg.anchor_image.empty()) {
      return;
    }
    const auto view = seg.anchor_image.view();
    if (view.width() <= 0 || view.height() <= 0) {
      return;
    }

    const std::string base_name =
        "strip_" + std::to_string(seg.start_ms) + "_" + std::to_string(seg.end_ms) + ".ppm";
    const std::filesystem::path final_path = strips_dir_ / base_name;
    try {
      write_ppm_atomic(final_path, view);
      const std::uint64_t idx = ++slot_index_;
      emit_fn_("timing_slot", nlohmann::json{
                                  {"index", idx},
                                  {"start_ms", seg.start_ms},
                                  {"end_ms", seg.end_ms},
                                  {"strip_path", final_path.string()},
                              });
    } catch (const std::exception& ex) {
      std::lock_guard<std::mutex> lock(err_mutex_);
      if (strip_write_error_.empty()) {
        strip_write_error_ = ex.what();
      }
    }
  }

  void on_subtitle_entry(const SubtitleEntry& entry) override {
    if (pure_timing_strips_) {
      return;
    }
    emit_fn_("subtitle_entry", subtitle_entry_to_json(entry));
  }

  void on_error(const ExtractionError& error) override {
    std::lock_guard<std::mutex> lock(err_mutex_);
    has_error_ = true;
    error_code_ = error.code;
    error_message_ = error.message;
  }

  [[nodiscard]] bool has_error() const {
    std::lock_guard<std::mutex> lock(err_mutex_);
    return has_error_;
  }

  [[nodiscard]] ExtractionErrorCode error_code() const {
    std::lock_guard<std::mutex> lock(err_mutex_);
    return error_code_;
  }

  [[nodiscard]] std::string error_message() const {
    std::lock_guard<std::mutex> lock(err_mutex_);
    return error_message_;
  }

  [[nodiscard]] std::string strip_write_error() const {
    std::lock_guard<std::mutex> lock(err_mutex_);
    return strip_write_error_;
  }

 private:
  [[maybe_unused]] std::uint64_t task_id_;
  bool pure_timing_strips_{false};
  std::filesystem::path strips_dir_;
  std::function<void(const std::string&, const nlohmann::json&)> emit_fn_;

  bool has_emitted_progress_{false};
  std::chrono::steady_clock::time_point first_progress_tp_{};
  std::chrono::steady_clock::time_point last_progress_tp_{};
  std::uint64_t progress_frames_{0};
  std::uint64_t slot_index_{0};

  mutable std::mutex err_mutex_;
  bool has_error_{false};
  ExtractionErrorCode error_code_{ExtractionErrorCode::Aborted};
  std::string error_message_{};
  std::string strip_write_error_{};
};

}  // namespace sublift::worker::detail

namespace sublift::worker {

struct WorkerServer::Impl {
  explicit Impl(IWorkerBackend& backend) : backend_(backend) {}

  void emit_json(std::ostream& out, const nlohmann::json& payload) {
    const std::string serialized =
        payload.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    std::lock_guard<std::mutex> lock(out_mutex_);
    out << serialized << '\n';
    out.flush();
  }

  void emit_event(
      std::ostream& out,
      std::uint64_t id,
      const std::string& event_name,
      const nlohmann::json& data) {
    emit_json(out, nlohmann::json{
                       {"id", id},
                       {"type", "event"},
                       {"event", event_name},
                       {"data", data},
                   });
  }

  void emit_ok(std::ostream& out, std::uint64_t id, const nlohmann::json& data) {
    emit_json(out, nlohmann::json{
                       {"id", id},
                       {"type", "result"},
                       {"ok", true},
                       {"data", data},
                   });
  }

  void emit_error(
      std::ostream& out,
      std::uint64_t id,
      const std::string& code,
      const std::string& message) {
    emit_json(out, nlohmann::json{
                       {"id", id},
                       {"type", "result"},
                       {"ok", false},
                       {"error",
                        {
                            {"code", code},
                            {"message", message},
                        }},
                   });
  }

  void reap_finished_tasks() {
    std::vector<std::shared_ptr<detail::TaskState>> to_join;
    {
      std::lock_guard<std::mutex> lock(tasks_mutex_);
      for (auto it = tasks_.begin(); it != tasks_.end();) {
        if (it->second->finished.load(std::memory_order_acquire)) {
          to_join.push_back(std::move(it->second));
          it = tasks_.erase(it);
        } else {
          ++it;
        }
      }
    }
    for (auto& t : to_join) {
      if (t->thread.joinable()) {
        t->thread.join();
      }
    }
  }

  void cancel_and_join_all() {
    std::unordered_map<std::uint64_t, std::shared_ptr<detail::TaskState>> snapshot;
    {
      std::lock_guard<std::mutex> lock(tasks_mutex_);
      snapshot.swap(tasks_);
    }
    for (auto& [_, t] : snapshot) {
      t->request_cancel();
    }
    for (auto& [_, t] : snapshot) {
      if (t->thread.joinable()) {
        t->thread.join();
      }
    }
  }

  void handle_capabilities(std::ostream& out, std::uint64_t id) {
    const CapabilitiesInfo caps = backend_.query_capabilities();
    emit_ok(out, id, nlohmann::json{
                         {"engines", caps.engines},
                         {"ffmpeg_available", caps.ffmpeg_available},
                         {"ffprobe_available", caps.ffprobe_available},
                         {"hwaccel_modes", caps.hwaccel_modes},
                         {"platform", caps.platform},
                     });
  }

  void handle_finalize_srt(
      std::ostream& out, std::uint64_t id, const nlohmann::json& params) {
    if (!params.contains("entries")) {
      emit_error(out, id, "InvalidRequest", "finalize_srt requires 'entries'");
      return;
    }
    std::vector<SubtitleEntry> entries;
    std::string parse_err;
    if (!detail::parse_subtitle_entries(params["entries"], entries, parse_err)) {
      emit_error(out, id, "InvalidRequest", parse_err);
      return;
    }

    if (params.contains("min_duration_ms") && !params["min_duration_ms"].is_null() &&
        !params["min_duration_ms"].is_number_integer()) {
      emit_error(out, id, "InvalidRequest", "'min_duration_ms' must be an integer");
      return;
    }
    if (params.contains("merge_gap_ms") && !params["merge_gap_ms"].is_null() &&
        !params["merge_gap_ms"].is_number_integer()) {
      emit_error(out, id, "InvalidRequest", "'merge_gap_ms' must be an integer");
      return;
    }

    const bool apply_merge =
        params.contains("apply_merge") && params["apply_merge"].is_boolean()
            ? params["apply_merge"].get<bool>()
            : false;
    const std::int64_t min_duration_ms =
        params.contains("min_duration_ms") && params["min_duration_ms"].is_number_integer()
            ? params["min_duration_ms"].get<std::int64_t>()
            : 500;
    const std::int64_t merge_gap_ms =
        params.contains("merge_gap_ms") && params["merge_gap_ms"].is_number_integer()
            ? params["merge_gap_ms"].get<std::int64_t>()
            : 1000;
    if (min_duration_ms < 0) {
      emit_error(out, id, "InvalidConfiguration", "min_duration_ms must be >= 0");
      return;
    }
    if (merge_gap_ms < 0) {
      emit_error(out, id, "InvalidConfiguration", "merge_gap_ms must be >= 0");
      return;
    }
    const bool drop_empty_text =
        params.contains("drop_empty_text") && params["drop_empty_text"].is_boolean()
            ? params["drop_empty_text"].get<bool>()
            : false;

    if (apply_merge) {
      for (auto& entry : entries) {
        entry.range.start_ms = std::max<std::int64_t>(0, entry.range.start_ms);
      }
      entries = sublift::text::merge_entries(
          entries, merge_gap_ms, min_duration_ms, drop_empty_text);
    }
    const auto [srt_text, cue_count] = sublift::text::format_srt_counted(entries);
    emit_ok(out, id, nlohmann::json{
                         {"srt_text", srt_text},
                         {"cue_count", cue_count},
                     });
  }

  void handle_cancel_task(
      std::ostream& out, std::uint64_t id, const nlohmann::json& params) {
    if (!params.contains("target_id") || !params["target_id"].is_number_unsigned()) {
      emit_error(out, id, "InvalidRequest", "cancel_task requires unsigned integer 'target_id'");
      return;
    }
    const std::uint64_t target_id = params["target_id"].get<std::uint64_t>();
    std::shared_ptr<detail::TaskState> target;
    {
      std::lock_guard<std::mutex> lock(tasks_mutex_);
      auto it = tasks_.find(target_id);
      if (it != tasks_.end() && !it->second->finished.load(std::memory_order_acquire)) {
        target = it->second;
      }
    }
    bool cancelled = false;
    if (target) {
      target->request_cancel();
      cancelled = true;
    }
    emit_ok(out, id, nlohmann::json{
                         {"target_id", target_id},
                         {"cancelled", cancelled},
                     });
  }

  void run_probe_video(
      std::ostream& out,
      const std::shared_ptr<detail::TaskState>& task,
      const nlohmann::json& params) {
    if (!params.contains("video_path") || !params["video_path"].is_string()) {
      emit_error(out, task->id, "InvalidRequest", "probe_video requires string 'video_path'");
      return;
    }
    const std::filesystem::path video_path(params["video_path"].get<std::string>());
    if (task->cancelled.load(std::memory_order_acquire)) {
      emit_error(out, task->id, "Aborted", "Task cancelled");
      return;
    }

    auto source = backend_.create_frame_source(1.0, std::nullopt, 0);
    task->bind_source(source.get());
    try {
      const VideoInfo info = source->probe(video_path);
      task->unbind_all();
      if (task->cancelled.load(std::memory_order_acquire) || source->is_cancelled()) {
        emit_error(out, task->id, "Aborted", "Task cancelled");
        return;
      }
      nlohmann::json data{
          {"width", info.width},
          {"height", info.height},
          {"duration_ms", info.duration_ms},
          {"fps", info.fps},
          {"display_transform_ok", info.display_transform_ok},
      };
      if (info.transform_note.has_value()) {
        data["transform_note"] = *info.transform_note;
      } else {
        data["transform_note"] = nullptr;
      }
      emit_ok(out, task->id, data);
    } catch (const std::exception& ex) {
      task->unbind_all();
      if (task->cancelled.load(std::memory_order_acquire) || source->is_cancelled()) {
        emit_error(out, task->id, "Aborted", "Task cancelled");
      } else {
        emit_error(out, task->id, "DecoderFailed", ex.what());
      }
    }
  }

  void run_extract_frame(
      std::ostream& out,
      const std::shared_ptr<detail::TaskState>& task,
      const nlohmann::json& params) {
    if (!params.contains("video_path") || !params["video_path"].is_string() ||
        !params.contains("out_path") || !params["out_path"].is_string() ||
        !params.contains("timestamp_ms") || !params["timestamp_ms"].is_number_integer()) {
      emit_error(out, task->id, "InvalidRequest",
                 "extract_frame requires 'video_path', 'timestamp_ms', and 'out_path'");
      return;
    }
    const std::filesystem::path video_path(params["video_path"].get<std::string>());
    const std::filesystem::path out_path(params["out_path"].get<std::string>());
    const std::int64_t timestamp_ms = params["timestamp_ms"].get<std::int64_t>();
    if (timestamp_ms < 0) {
      emit_error(out, task->id, "InvalidConfiguration", "timestamp_ms must be >= 0");
      return;
    }

    const std::string ext = detail::to_lower_ascii(out_path.extension().string());
    if (ext != ".png" && ext != ".ppm" && ext != ".pgm") {
      emit_error(out, task->id, "InvalidConfiguration",
                 "out_path must end with .png or .ppm/.pgm");
      return;
    }

    std::optional<SourceBox> crop;
    if (params.contains("crop") && !params["crop"].is_null()) {
      SourceBox box;
      std::string err;
      if (!detail::parse_source_box(params["crop"], box, err)) {
        emit_error(out, task->id, "InvalidConfiguration", err);
        return;
      }
      crop = box;
    }

    std::int32_t max_width = 0;
    if (params.contains("max_width") && !params["max_width"].is_null()) {
      if (!params["max_width"].is_number_integer()) {
        emit_error(out, task->id, "InvalidConfiguration", "max_width must be an integer");
        return;
      }
      max_width = params["max_width"].get<std::int32_t>();
      if (max_width < 0) {
        emit_error(out, task->id, "InvalidConfiguration", "max_width must be >= 0");
        return;
      }
    }

    if (task->cancelled.load(std::memory_order_acquire)) {
      emit_error(out, task->id, "Aborted", "Task cancelled");
      return;
    }

    auto source = backend_.create_frame_source(1.0, std::nullopt, 0);
    task->bind_source(source.get());
    ImageBuffer img;
    try {
      img = source->extract_frame(video_path, timestamp_ms, crop);
      task->unbind_all();
    } catch (const std::exception& ex) {
      task->unbind_all();
      if (task->cancelled.load(std::memory_order_acquire) || source->is_cancelled()) {
        emit_error(out, task->id, "Aborted", "Task cancelled");
      } else {
        emit_error(out, task->id, "DecoderFailed", ex.what());
      }
      return;
    }

    if (task->cancelled.load(std::memory_order_acquire) || source->is_cancelled()) {
      emit_error(out, task->id, "Aborted", "Task cancelled");
      return;
    }
    if (img.empty()) {
      emit_error(out, task->id, "DecoderFailed", "extract_frame returned empty image");
      return;
    }
    if (max_width > 0 && img.width() > max_width) {
      img = resize_max_width(img.view(), max_width);
    }
    try {
      if (ext == ".png") {
        write_png_atomic(out_path, img.view());
      } else {
        write_ppm_atomic(out_path, img.view());
      }
      emit_ok(out, task->id, nlohmann::json{
                                 {"out_path", out_path.string()},
                                 {"width", img.width()},
                                 {"height", img.height()},
                             });
    } catch (const std::exception& ex) {
      emit_error(out, task->id, "IoError", ex.what());
    }
  }

  void run_detect_auto_region(
      std::ostream& out,
      const std::shared_ptr<detail::TaskState>& task,
      const nlohmann::json& params) {
    if (!params.contains("video_path") || !params["video_path"].is_string() ||
        !params.contains("engine") || !params["engine"].is_string()) {
      emit_error(out, task->id, "InvalidRequest",
                 "detect_auto_region requires 'video_path' and 'engine'");
      return;
    }
    const std::filesystem::path video_path(params["video_path"].get<std::string>());
    const std::string engine_id = params["engine"].get<std::string>();
    const std::string script =
        params.contains("script") && params["script"].is_string()
            ? params["script"].get<std::string>()
            : std::string(SCRIPT_AUTO);
    if (!is_valid_script(script)) {
      emit_error(out, task->id, "InvalidConfiguration", "Invalid script: " + script);
      return;
    }
    const double bottom_ratio =
        params.contains("bottom_ratio") && params["bottom_ratio"].is_number()
            ? params["bottom_ratio"].get<double>()
            : 0.45;
    if (!std::isfinite(bottom_ratio) || bottom_ratio <= 0.0 || bottom_ratio > 1.0) {
      emit_error(out, task->id, "InvalidConfiguration",
                 "bottom_ratio must be in (0.0, 1.0]");
      return;
    }

    auto source = backend_.create_frame_source(1.0, std::nullopt, 0);
    task->bind_source(source.get());

    std::int32_t ref_width = 1920;
    try {
      const VideoInfo info = source->probe(video_path);
      if (info.width > 0) {
        ref_width = info.width;
      }
    } catch (const std::exception& ex) {
      task->unbind_all();
      if (task->cancelled.load(std::memory_order_acquire) || source->is_cancelled()) {
        emit_error(out, task->id, "Aborted", "Task cancelled");
      } else {
        emit_error(out, task->id, "DecoderFailed", ex.what());
      }
      return;
    }

    if (task->cancelled.load(std::memory_order_acquire) || source->is_cancelled()) {
      task->unbind_all();
      emit_error(out, task->id, "Aborted", "Task cancelled");
      return;
    }

    const OcrAcquireResult ocr = backend_.acquire_ocr_engine(engine_id, ref_width);
    if (!ocr.ok()) {
      task->unbind_all();
      emit_error(out, task->id, ocr.error_code, ocr.error_message);
      return;
    }

    try {
      AutoRegionOptions opts;
      opts.script = script;
      opts.bottom_ratio = bottom_ratio;
      const AutoRegionResult res =
          detect_auto_region(*source, *ocr.engine, video_path, opts);
      task->unbind_all();
      if (task->cancelled.load(std::memory_order_acquire) || source->is_cancelled()) {
        emit_error(out, task->id, "Aborted", "Task cancelled");
        return;
      }
      nlohmann::json profile_json = nullptr;
      if (res.profile.height > 0 || res.profile.y_max > 0) {
        profile_json = detail::profile_to_json(res.profile);
      }
      emit_ok(out, task->id, nlohmann::json{
                                 {"detected", res.detected},
                                 {"region_box", detail::source_box_to_json(res.region_box)},
                                 {"profile", profile_json},
                                 {"sample_timestamp_ms", res.sample_timestamp_ms},
                                 {"preview_text", res.preview_text},
                                 {"confidence", res.confidence},
                                 {"error_message", res.error_message},
                             });
    } catch (const std::exception& ex) {
      task->unbind_all();
      if (task->cancelled.load(std::memory_order_acquire) || source->is_cancelled()) {
        emit_error(out, task->id, "Aborted", "Task cancelled");
      } else {
        emit_error(out, task->id, "OcrFailed", ex.what());
      }
    }
  }

  void run_extraction_task(
      std::ostream& out,
      const std::shared_ptr<detail::TaskState>& task,
      const nlohmann::json& params) {
    if (!params.contains("video_path") || !params["video_path"].is_string() ||
        !params.contains("mode") || !params["mode"].is_string()) {
      emit_error(out, task->id, "InvalidRequest",
                 "run_extraction requires 'video_path' and 'mode'");
      return;
    }
    const std::filesystem::path video_path(params["video_path"].get<std::string>());
    const std::string mode = params["mode"].get<std::string>();
    if (mode != "local_ocr" && mode != "pure_timing_strips") {
      emit_error(out, task->id, "InvalidConfiguration",
                 "mode must be 'local_ocr' or 'pure_timing_strips'");
      return;
    }
    const bool pure_timing_strips = (mode == "pure_timing_strips");

    std::string engine_id;
    if (!pure_timing_strips) {
      if (!params.contains("engine") || !params["engine"].is_string()) {
        emit_error(out, task->id, "InvalidConfiguration",
                   "run_extraction in local_ocr mode requires 'engine'");
        return;
      }
      engine_id = params["engine"].get<std::string>();
    }

    std::filesystem::path strips_dir;
    if (pure_timing_strips) {
      if (!params.contains("strips_dir") || !params["strips_dir"].is_string() ||
          params["strips_dir"].get<std::string>().empty()) {
        emit_error(out, task->id, "InvalidConfiguration",
                   "pure_timing_strips mode requires non-empty 'strips_dir'");
        return;
      }
      strips_dir = params["strips_dir"].get<std::string>();
      std::error_code ec;
      std::filesystem::create_directories(strips_dir, ec);
      if (ec) {
        emit_error(out, task->id, "SourceReadError",
                   "Failed to create strips_dir: " + ec.message());
        return;
      }
    }

    std::optional<SourceBox> region_box;
    if (params.contains("region_box") && !params["region_box"].is_null()) {
      SourceBox box;
      std::string err;
      if (!detail::parse_source_box(params["region_box"], box, err)) {
        emit_error(out, task->id, "InvalidConfiguration", err);
        return;
      }
      region_box = box;
    }

    ExtractionConfig cfg;
    if (params.contains("script") && params["script"].is_string()) {
      cfg.subtitle_script = params["script"].get<std::string>();
    }
    if (params.contains("config") && params["config"].is_object()) {
      const auto& c = params["config"];
      if (c.contains("sample_fps") && c["sample_fps"].is_number()) {
        cfg.sample_fps = c["sample_fps"].get<double>();
      }
      if (c.contains("region_bottom_ratio") && c["region_bottom_ratio"].is_number()) {
        cfg.region_bottom_ratio = c["region_bottom_ratio"].get<double>();
      }
      if (c.contains("subtitle_script") && c["subtitle_script"].is_string()) {
        cfg.subtitle_script = c["subtitle_script"].get<std::string>();
      }
      if (c.contains("confidence_threshold") && c["confidence_threshold"].is_number()) {
        cfg.confidence_threshold = c["confidence_threshold"].get<double>();
      }
      if (c.contains("min_duration_ms") && c["min_duration_ms"].is_number_integer()) {
        cfg.min_duration_ms = c["min_duration_ms"].get<std::int64_t>();
      }
      if (c.contains("merge_gap_ms") && c["merge_gap_ms"].is_number_integer()) {
        cfg.merge_gap_ms = c["merge_gap_ms"].get<std::int64_t>();
      }
      if (c.contains("drop_empty_text") && c["drop_empty_text"].is_boolean()) {
        cfg.drop_empty_text = c["drop_empty_text"].get<bool>();
      }
      if (c.contains("num_threads") && c["num_threads"].is_number_unsigned()) {
        cfg.num_threads = c["num_threads"].get<std::size_t>();
      }
      if (c.contains("queue_capacity") && c["queue_capacity"].is_number_unsigned()) {
        cfg.queue_capacity = c["queue_capacity"].get<std::size_t>();
      }
      if (c.contains("predetected_profile") && c["predetected_profile"].is_object()) {
        const auto& p = c["predetected_profile"];
        SubtitleProfile prof;
        if (p.contains("script") && p["script"].is_string()) {
          prof.script = p["script"].get<std::string>();
        }
        if (p.contains("center_x") && p["center_x"].is_number_integer()) {
          prof.center_x = p["center_x"].get<std::int32_t>();
        }
        if (p.contains("center_y") && p["center_y"].is_number_integer()) {
          prof.center_y = p["center_y"].get<std::int32_t>();
        }
        if (p.contains("height") && p["height"].is_number_integer()) {
          prof.height = p["height"].get<std::int32_t>();
        }
        if (p.contains("y_min") && p["y_min"].is_number_integer()) {
          prof.y_min = p["y_min"].get<std::int32_t>();
        }
        if (p.contains("y_max") && p["y_max"].is_number_integer()) {
          prof.y_max = p["y_max"].get<std::int32_t>();
        }
        cfg.predetected_profile = prof;
      }
    }

    if (!std::isfinite(cfg.sample_fps) || cfg.sample_fps <= 0.0) {
      emit_error(out, task->id, "InvalidConfiguration", "sample_fps must be > 0");
      return;
    }
    if (!std::isfinite(cfg.region_bottom_ratio) ||
        cfg.region_bottom_ratio <= 0.0 || cfg.region_bottom_ratio >= 1.0) {
      emit_error(out, task->id, "InvalidConfiguration",
                 "region_bottom_ratio must be in (0.0, 1.0)");
      return;
    }
    if (!is_valid_script(cfg.subtitle_script)) {
      emit_error(out, task->id, "InvalidConfiguration",
                 "Invalid subtitle_script: " + cfg.subtitle_script);
      return;
    }
    if (!std::isfinite(cfg.confidence_threshold) ||
        cfg.confidence_threshold < 0.0 || cfg.confidence_threshold > 1.0) {
      emit_error(out, task->id, "InvalidConfiguration",
                 "confidence_threshold must be in [0.0, 1.0]");
      return;
    }
    if (cfg.min_duration_ms < 0) {
      emit_error(out, task->id, "InvalidConfiguration", "min_duration_ms must be >= 0");
      return;
    }
    if (cfg.merge_gap_ms < 0) {
      emit_error(out, task->id, "InvalidConfiguration", "merge_gap_ms must be >= 0");
      return;
    }

    std::optional<std::string> hwaccel("auto");
    if (params.contains("hwaccel") && params["hwaccel"].is_string()) {
      hwaccel = params["hwaccel"].get<std::string>();
    }

    if (pure_timing_strips) {
      cfg.pure_timing = true;
      cfg.retain_midpoint_in_pure_timing = true;
      cfg.min_duration_ms = 0;
      cfg.drop_empty_text = false;
      cfg.queue_capacity = std::max<std::size_t>(cfg.queue_capacity, 32);
    } else {
      cfg.pure_timing = false;
      cfg.retain_midpoint_in_pure_timing = false;
    }

    if (task->cancelled.load(std::memory_order_acquire)) {
      emit_error(out, task->id, "Aborted", "Task cancelled");
      return;
    }

    // Create source upfront with expected max_output_width (1920 for pure_timing_strips)
    std::int32_t initial_max_width = pure_timing_strips ? 1920 : 0;
    auto source = backend_.create_frame_source(cfg.sample_fps, hwaccel, initial_max_width);
    task->bind_source(source.get());
    struct SourceUnbindGuard {
      detail::TaskState& state;
      ~SourceUnbindGuard() { state.unbind_all(); }
    } source_unbind_guard{*task};

    VideoInfo video_info;
    try {
      video_info = source->probe(video_path);
    } catch (const std::exception& ex) {
      task->unbind_all();
      if (task->cancelled.load(std::memory_order_acquire) || source->is_cancelled()) {
        emit_error(out, task->id, "Aborted", "Task cancelled");
      } else {
        emit_error(out, task->id, "DecoderFailed", ex.what());
      }
      return;
    }

    if (task->cancelled.load(std::memory_order_acquire) || source->is_cancelled()) {
      task->unbind_all();
      emit_error(out, task->id, "Aborted", "Task cancelled");
      return;
    }

    // Acquire OCR engine in local_ocr mode before configuring final session
    std::shared_ptr<IOcrEngine> ocr_holder;
    detail::NullOcrEngine null_ocr;
    if (!pure_timing_strips) {
      const OcrAcquireResult ocr =
          backend_.acquire_ocr_engine(engine_id, video_info.width);
      if (!ocr.ok()) {
        task->unbind_all();
        emit_error(out, task->id, ocr.error_code, ocr.error_message);
        return;
      }
      ocr_holder = ocr.engine;
    }

    // Resolve effective_region_box (honoring custom region_bottom_ratio when region_box is omitted)
    std::optional<SourceBox> effective_region_box = region_box;
    if (effective_region_box.has_value() && video_info.width > 0 && video_info.height > 0) {
      if (effective_region_box->right() > video_info.width ||
          effective_region_box->bottom() > video_info.height) {
        task->unbind_all();
        emit_error(out, task->id, "InvalidConfiguration",
                   "region_box exceeds probed video dimensions");
        return;
      }
    } else if (!effective_region_box.has_value() && video_info.display_transform_ok &&
               video_info.width > 0 && video_info.height > 0) {
      const std::int32_t raw_h =
          static_cast<std::int32_t>(std::lround(video_info.height * cfg.region_bottom_ratio));
      std::int32_t crop_h = std::max<std::int32_t>(2, (raw_h / 2) * 2);
      crop_h = std::clamp<std::int32_t>(crop_h, 1, video_info.height);
      const std::int32_t crop_y = std::max<std::int32_t>(0, video_info.height - crop_h);
      effective_region_box = SourceBox{0, crop_y, video_info.width, crop_h};
    }

    // Disable max_output_width only if region_box is active under non-identity display transform
    if (pure_timing_strips && effective_region_box.has_value() &&
        !video_info.display_transform_ok && initial_max_width > 0) {
      auto next_source = backend_.create_frame_source(cfg.sample_fps, hwaccel, 0);
      task->bind_source(next_source.get());
      source = std::move(next_source);
    }

    std::unique_ptr<IDetector> detector;
    if (!video_info.display_transform_ok) {
      if (effective_region_box.has_value()) {
        detector = std::make_unique<sublift::adapters::ffmpeg::FixedRegionDetector>(
            FrameLocalBox{effective_region_box->x,
                          effective_region_box->y,
                          effective_region_box->width,
                          effective_region_box->height});
      } else {
        detector = std::make_unique<sublift::adapters::ffmpeg::BottomCropDetector>(
            cfg.region_bottom_ratio);
      }
    }

    // Declare observer BEFORE session so ~ExtractionSession() joins all background threads
    // (SegmentRecognizer::worker_ and consumer_thread_) before ~WorkerExtractionObserver().
    detail::WorkerExtractionObserver observer(
        task->id,
        pure_timing_strips,
        strips_dir,
        [this, &out, task](const std::string& ev, const nlohmann::json& data) {
          emit_event(out, task->id, ev, data);
        });

    IOcrEngine& active_ocr =
        pure_timing_strips ? static_cast<IOcrEngine&>(null_ocr) : *ocr_holder;

    std::vector<SubtitleEntry> entries;
    ExtractionMetrics metrics{};
    bool session_cancelled = false;
    bool source_cancelled = false;
    bool threw = false;
    std::string exception_message;

    {
      ExtractionSession session(*source, active_ocr, detector.get(), nullptr, cfg);
      session.set_observer(&observer);
      task->bind_session(&session);
      struct SessionUnbindGuard {
        detail::TaskState& state;
        ~SessionUnbindGuard() { state.unbind_all(); }
      } session_unbind_guard{*task};

      try {
        entries = session.process_video(video_path, effective_region_box);
        metrics = session.metrics();
        session_cancelled = session.is_cancelled();
        source_cancelled = source->is_cancelled();
      } catch (const std::exception& ex) {
        threw = true;
        exception_message = ex.what();
        session_cancelled = session.is_cancelled();
        source_cancelled = source->is_cancelled();
      }
    }

    if (task->cancelled.load(std::memory_order_acquire) ||
        session_cancelled ||
        source_cancelled) {
      emit_error(out, task->id, "Aborted", "Task cancelled");
      return;
    }

    if (threw) {
      if (observer.has_error()) {
        emit_error(out,
                   task->id,
                   detail::error_code_to_string(observer.error_code()),
                   observer.error_message());
      } else {
        emit_error(out, task->id, "DecoderFailed", exception_message);
      }
      return;
    }

    if (!observer.strip_write_error().empty()) {
      emit_error(out, task->id, "SourceReadError", observer.strip_write_error());
      return;
    }
    if (observer.has_error()) {
      emit_error(out,
                 task->id,
                 detail::error_code_to_string(observer.error_code()),
                 observer.error_message());
      return;
    }

    nlohmann::json entries_json = nlohmann::json::array();
    for (const auto& entry : entries) {
      entries_json.push_back(detail::subtitle_entry_to_json(entry));
    }
    emit_ok(out, task->id, nlohmann::json{
                               {"entries", entries_json},
                               {"metrics", detail::metrics_to_json(metrics)},
                           });
  }

  void run_recognize_strips(
      std::ostream& out,
      const std::shared_ptr<detail::TaskState>& task,
      const nlohmann::json& params) {
    if (!params.contains("engine") || !params["engine"].is_string() ||
        !params.contains("strip_paths") || !params["strip_paths"].is_array()) {
      emit_error(out, task->id, "InvalidRequest",
                 "recognize_strips requires 'engine' and array 'strip_paths'");
      return;
    }
    const std::string engine_id = params["engine"].get<std::string>();
    const std::string script =
        params.contains("script") && params["script"].is_string()
            ? params["script"].get<std::string>()
            : std::string(SCRIPT_AUTO);
    if (!is_valid_script(script)) {
      emit_error(out, task->id, "InvalidConfiguration", "Invalid script: " + script);
      return;
    }

    std::vector<std::string> strip_paths;
    for (const auto& item : params["strip_paths"]) {
      if (!item.is_string()) {
        emit_error(out, task->id, "InvalidRequest",
                   "All entries in 'strip_paths' must be strings");
        return;
      }
      strip_paths.push_back(item.get<std::string>());
    }

    const OcrAcquireResult ocr = backend_.acquire_ocr_engine(engine_id, 960);
    if (!ocr.ok()) {
      emit_error(out, task->id, ocr.error_code, ocr.error_message);
      return;
    }

    nlohmann::json results_json = nlohmann::json::array();
    for (const auto& path_str : strip_paths) {
      if (task->cancelled.load(std::memory_order_acquire)) {
        emit_error(out, task->id, "Aborted", "Task cancelled");
        return;
      }

      try {
        ImageBuffer loaded = load_ppm(path_str);
        ImageBuffer rgb_buf = ensure_rgb24(loaded.view());
        const OcrResult ocr_res = ocr.engine->recognize(rgb_buf.view());

        std::string text;
        double confidence = 0.0;
        if (!ocr_res.lines.empty()) {
          const SubtitleProfile profile = SubtitleProfile::from_crop(
              rgb_buf.width(), rgb_buf.height(), script);
          const auto selected = sublift::text::select_line(ocr_res.lines, profile);
          if (selected.has_value()) {
            text = sublift::text::cleanup_subtitle_text(selected->text, script);
            confidence = selected->confidence;
          }
        } else {
          text = sublift::text::cleanup_subtitle_text(ocr_res.text, script);
          confidence = ocr_res.confidence;
        }
        results_json.push_back(nlohmann::json{
            {"strip_path", path_str},
            {"ok", true},
            {"text", text},
            {"confidence", confidence},
        });
      } catch (const std::exception& ex) {
        results_json.push_back(nlohmann::json{
            {"strip_path", path_str},
            {"ok", false},
            {"text", ""},
            {"confidence", 0.0},
            {"error_message", ex.what()},
        });
      }
    }

    emit_ok(out, task->id, nlohmann::json{{"results", results_json}});
  }

  void dispatch_async(
      std::ostream& out,
      std::uint64_t id,
      const std::string& op,
      nlohmann::json params) {
    reap_finished_tasks();

    auto task = std::make_shared<detail::TaskState>();
    task->id = id;

    {
      std::lock_guard<std::mutex> lock(tasks_mutex_);
      if (tasks_.find(id) != tasks_.end()) {
        emit_error(out, id, "InvalidRequest",
                   "Duplicate active request id: " + std::to_string(id));
        return;
      }
      tasks_.emplace(id, task);
    }

    task->thread = std::thread([this, &out, task, op, p = std::move(params)]() {
      struct FinishGuard {
        std::shared_ptr<detail::TaskState> t;
        ~FinishGuard() { t->finished.store(true, std::memory_order_release); }
      } guard{task};

      try {
        if (op == "probe_video") {
          run_probe_video(out, task, p);
        } else if (op == "extract_frame") {
          run_extract_frame(out, task, p);
        } else if (op == "detect_auto_region") {
          run_detect_auto_region(out, task, p);
        } else if (op == "run_extraction") {
          run_extraction_task(out, task, p);
        } else if (op == "recognize_strips") {
          run_recognize_strips(out, task, p);
        }
      } catch (const std::exception& ex) {
        emit_error(out, task->id, "InternalError", ex.what());
      } catch (...) {
        emit_error(out, task->id, "InternalError", "Unknown non-standard exception");
      }
    });
  }

  IWorkerBackend& backend_;
  std::mutex out_mutex_;
  std::mutex tasks_mutex_;
  std::unordered_map<std::uint64_t, std::shared_ptr<detail::TaskState>> tasks_;
};

WorkerServer::WorkerServer(IWorkerBackend& backend)
    : impl_(std::make_unique<Impl>(backend)) {}

WorkerServer::~WorkerServer() {
  if (impl_) {
    impl_->cancel_and_join_all();
  }
}

int WorkerServer::run(std::istream& in, std::ostream& out) {
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    const auto first_non_ws = line.find_first_not_of(" \t\r\n");
    if (first_non_ws == std::string::npos) {
      continue;
    }

    nlohmann::json req;
    try {
      req = nlohmann::json::parse(line);
    } catch (const std::exception& ex) {
      impl_->emit_error(out, 0, "InvalidRequest",
                        std::string("Malformed JSON: ") + ex.what());
      continue;
    }

    if (!req.is_object() ||
        !req.contains("id") || !req["id"].is_number_unsigned() ||
        !req.contains("op") || !req["op"].is_string()) {
      const std::uint64_t err_id =
          (req.is_object() && req.contains("id") && req["id"].is_number_unsigned())
              ? req["id"].get<std::uint64_t>()
              : 0;
      impl_->emit_error(out, err_id, "InvalidRequest",
                        "Request must be an object with unsigned 'id' and string 'op'");
      continue;
    }

    const std::uint64_t id = req["id"].get<std::uint64_t>();
    const std::string op = req["op"].get<std::string>();
    nlohmann::json params = nlohmann::json::object();
    if (req.contains("params")) {
      if (!req["params"].is_object()) {
        impl_->emit_error(out, id, "InvalidRequest", "'params' must be a JSON object");
        continue;
      }
      params = req["params"];
    }

    if (op == "cancel_task") {
      impl_->handle_cancel_task(out, id, params);
    } else if (op == "capabilities") {
      impl_->handle_capabilities(out, id);
    } else if (op == "finalize_srt") {
      impl_->handle_finalize_srt(out, id, params);
    } else if (op == "probe_video" ||
               op == "extract_frame" ||
               op == "detect_auto_region" ||
               op == "run_extraction" ||
               op == "recognize_strips") {
      impl_->dispatch_async(out, id, op, std::move(params));
    } else {
      impl_->emit_error(out, id, "InvalidRequest", "Unknown operation: '" + op + "'");
    }
  }

  // stdin reached EOF: cooperatively cancel all active tasks and join threads cleanly.
  impl_->cancel_and_join_all();
  return 0;
}

}  // namespace sublift::worker
