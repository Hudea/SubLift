#include "sublift/server/routes.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <httplib.h>

#include "sublift/adapters/ffmpeg.hpp"
#include "sublift/adapters/paddle.hpp"
#include "sublift/adapters/vision.hpp"
#include "sublift/models/resource_locator.hpp"
#include "sublift/version.hpp"

namespace sublift::server {

namespace {

struct ScopedFileDescriptor {
  int fd{-1};
  explicit ScopedFileDescriptor(int descriptor) : fd(descriptor) {}
  ~ScopedFileDescriptor() {
    if (fd >= 0) {
      ::close(fd);
    }
  }
  ScopedFileDescriptor(const ScopedFileDescriptor&) = delete;
  ScopedFileDescriptor& operator=(const ScopedFileDescriptor&) = delete;
};

bool is_blank_text(std::string_view text) {
  for (unsigned char c : text) {
    if (!std::isspace(c)) return false;
  }
  return true;
}

}  // namespace

std::string format_srt_timestamp(std::int64_t ms) {
  if (ms < 0) ms = 0;
  const auto total_ms = ms;
  const auto hours = total_ms / 3'600'000;
  const auto minutes = (total_ms % 3'600'000) / 60'000;
  const auto seconds = (total_ms % 60'000) / 1000;
  const auto millis = total_ms % 1000;

  char buf[32];
  std::snprintf(buf, sizeof(buf), "%02lld:%02lld:%02lld,%03lld",
                static_cast<long long>(hours),
                static_cast<long long>(minutes),
                static_cast<long long>(seconds),
                static_cast<long long>(millis));
  return std::string{buf};
}

std::string format_entries_to_srt(const std::vector<sublift::SubtitleEntry>& entries) {
  if (entries.empty()) return "";

  std::ostringstream oss;
  std::size_t idx = 1;

  for (const auto& e : entries) {
    if (is_blank_text(e.text)) {
      continue;
    }
    std::int64_t start = std::max<std::int64_t>(0, e.start_ms);
    std::int64_t end = std::max<std::int64_t>(start, e.end_ms);

    oss << idx++ << "\n";
    oss << format_srt_timestamp(start) << " --> " << format_srt_timestamp(end) << "\n";
    oss << e.text << "\n\n";
  }

  return oss.str();
}

nlohmann::json SystemInfoDTO::to_json() const {
  nlohmann::json root;
  root["version"] = version;
  root["runtime"] = runtime;
  root["capabilities"] = capabilities;

  nlohmann::json engines_arr = nlohmann::json::array();
  for (const auto& eng : engines) {
    nlohmann::json e;
    e["name"] = eng.name;
    e["available"] = eng.available;
    e["detail"] = eng.detail;
    if (!eng.model_type.empty()) {
      e["model_type"] = eng.model_type;
    }
    if (!eng.model_root.empty()) {
      e["model_root"] = eng.model_root;
    }
    engines_arr.push_back(e);
  }
  root["engines"] = engines_arr;

  nlohmann::json ffmpeg_obj;
  ffmpeg_obj["available"] = ffmpeg_available;
  if (!ffmpeg_path.empty()) {
    ffmpeg_obj["path"] = ffmpeg_path;
  }
  root["ffmpeg"] = ffmpeg_obj;

  return root;
}

SystemInfoDTO collect_system_info() {
  SystemInfoDTO info;
  info.version = std::string(sublift::version());
  info.runtime = "cpp";
  info.capabilities = {
      "path_mode",
      "frame_mode",
      "push_entry",
      "cancel",
      "video_stream",
      "video_frame",
      "job_schedule",
      "sse_stream",
      "srt_export",
  };

  // 1. Apple Vision engine
  bool vision_avail = sublift::is_vision_available();
  info.engines.push_back({
      .name = "vision",
      .available = vision_avail,
      .detail = vision_avail ? "Apple Vision Framework OCR (macOS)"
                             : "Not supported or not compiled on this platform",
      .model_type = "",
      .model_root = "",
  });

  // 2. PaddleOCR engine
  auto paddle_cap = sublift::PaddleOcrEngine::probe_capabilities();
  info.engines.push_back({
      .name = "paddle",
      .available = paddle_cap.available,
      .detail = paddle_cap.detail,
      .model_type = paddle_cap.model_type,
      .model_root = paddle_cap.model_root,
  });

  // 3. Mock engine (testing / diagnostic)
  info.engines.push_back({
      .name = "mock",
      .available = true,
      .detail = "Mock Engine for testing and validation",
      .model_type = "",
      .model_root = "",
  });

  // 4. FFmpeg toolchain
  sublift::models::ResourceLocator locator;
  auto ffmpeg_res = locator.locate_ffmpeg_executable();
  info.ffmpeg_available = ffmpeg_res.found;
  if (ffmpeg_res.found) {
    info.ffmpeg_path = ffmpeg_res.value.string();
  }

  return info;
}

std::string get_video_mime_type(const std::filesystem::path& path) {
  std::string ext = path.extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });

  if (ext == ".mp4" || ext == ".m4v") return "video/mp4";
  if (ext == ".webm") return "video/webm";
  if (ext == ".mov") return "video/quicktime";
  if (ext == ".mkv") return "video/x-matroska";
  if (ext == ".avi") return "video/x-msvideo";
  if (ext == ".flv") return "video/x-flv";
  return "video/mp4";
}

void register_routes(httplib::Server& server,
                     std::shared_ptr<JobManager> job_manager,
                     const std::string& static_dir) {
  // CORS Preflight
  server.Options(R"(/api/.*)", [](const httplib::Request&, httplib::Response& res) {
    res.status = 204;
  });

  // GET /api/system/info
  server.Get("/api/system/info", [](const httplib::Request&, httplib::Response& res) {
    auto info = collect_system_info();
    res.set_content(info.to_json().dump(), "application/json; charset=utf-8");
  });

  // GET /api/video/frame?path=<video_path>&time_s=<time_s>
  server.Get("/api/video/frame", [](const httplib::Request& req, httplib::Response& res) {
    if (!req.has_param("path")) {
      res.status = 400;
      nlohmann::json err = {{"error", "Missing 'path' query parameter"}};
      res.set_content(err.dump(), "application/json; charset=utf-8");
      return;
    }

    std::filesystem::path video_path = req.get_param_value("path");
    std::error_code ec;
    if (!std::filesystem::exists(video_path, ec) || !std::filesystem::is_regular_file(video_path, ec)) {
      res.status = 404;
      nlohmann::json err = {{"error", "Video file not found: " + video_path.string()}};
      res.set_content(err.dump(), "application/json; charset=utf-8");
      return;
    }

    double time_s = 0.0;
    if (req.has_param("time_s")) {
      try {
        time_s = std::stod(req.get_param_value("time_s"));
      } catch (...) {
        time_s = 0.0;
      }
    }

    try {
      auto jpeg_bytes = sublift::ffmpeg::extract_single_frame_jpeg(video_path, time_s);
      res.set_content(reinterpret_cast<const char*>(jpeg_bytes.data()),
                      jpeg_bytes.size(), "image/jpeg");
    } catch (const std::exception& e) {
      res.status = 500;
      nlohmann::json err = {{"error", std::string("Frame extraction failed: ") + e.what()}};
      res.set_content(err.dump(), "application/json; charset=utf-8");
    }
  });

  // GET /api/video/stream?path=<video_path>
  server.Get("/api/video/stream", [](const httplib::Request& req, httplib::Response& res) {
    if (!req.has_param("path")) {
      res.status = 400;
      nlohmann::json err = {{"error", "Missing 'path' query parameter"}};
      res.set_content(err.dump(), "application/json; charset=utf-8");
      return;
    }

    std::filesystem::path video_path = req.get_param_value("path");
    std::error_code ec;
    if (!std::filesystem::exists(video_path, ec) || !std::filesystem::is_regular_file(video_path, ec)) {
      res.status = 404;
      nlohmann::json err = {{"error", "Video file not found: " + video_path.string()}};
      res.set_content(err.dump(), "application/json; charset=utf-8");
      return;
    }

    const std::uint64_t total_size = std::filesystem::file_size(video_path, ec);
    if (total_size == 0) {
      res.status = 200;
      res.set_header("Content-Length", "0");
      res.set_header("Accept-Ranges", "bytes");
      return;
    }

    const std::string mime_type = get_video_mime_type(video_path);
    res.set_header("Accept-Ranges", "bytes");

    int fd = ::open(video_path.c_str(), O_RDONLY);
    if (fd < 0) {
      res.status = 500;
      nlohmann::json err = {{"error", "Failed to open video file on server"}};
      res.set_content(err.dump(), "application/json; charset=utf-8");
      return;
    }

    auto shared_fd = std::make_shared<ScopedFileDescriptor>(fd);

    res.set_content_provider(
        total_size,
        mime_type.c_str(),
        [shared_fd](size_t offset, size_t length, httplib::DataSink& sink) {
          constexpr size_t BUF_SIZE = 64 * 1024;
          std::vector<char> buffer(BUF_SIZE);
          size_t cur_offset = offset;
          size_t remain = length;

          while (remain > 0) {
            size_t to_read = std::min(remain, BUF_SIZE);
            ssize_t bytes_read = ::pread(shared_fd->fd, buffer.data(), to_read, static_cast<off_t>(cur_offset));
            if (bytes_read <= 0) {
              return false;
            }
            if (!sink.write(buffer.data(), static_cast<size_t>(bytes_read))) {
              return false;
            }
            cur_offset += static_cast<size_t>(bytes_read);
            remain -= static_cast<size_t>(bytes_read);
          }
          return true;
        });
  });

  // POST /api/jobs (Create and start a subtitle extraction job)
  server.Post("/api/jobs", [job_manager](const httplib::Request& req, httplib::Response& res) {
    if (!job_manager) {
      res.status = 500;
      nlohmann::json err = {{"error", "JobManager is not initialized"}};
      res.set_content(err.dump(), "application/json; charset=utf-8");
      return;
    }

    try {
      auto body_json = nlohmann::json::parse(req.body);
      if (!body_json.contains("video_path") || !body_json["video_path"].is_string() ||
          body_json["video_path"].get<std::string>().empty()) {
        res.status = 400;
        nlohmann::json err = {{"error", "Missing or invalid 'video_path'"}};
        res.set_content(err.dump(), "application/json; charset=utf-8");
        return;
      }

      JobConfig cfg;
      cfg.video_path = body_json["video_path"].get<std::string>();
      if (body_json.contains("engine") && body_json["engine"].is_string()) {
        cfg.engine = body_json["engine"].get<std::string>();
      }
      if (body_json.contains("fps") && body_json["fps"].is_number()) {
        cfg.fps = body_json["fps"].get<double>();
      }
      if (body_json.contains("confidence_threshold") && body_json["confidence_threshold"].is_number()) {
        cfg.confidence_threshold = body_json["confidence_threshold"].get<double>();
      }
      if (body_json.contains("region_box") && body_json["region_box"].is_object()) {
        cfg.region_box = RegionBox::from_json(body_json["region_box"]);
      }

      std::error_code ec;
      if (!std::filesystem::exists(cfg.video_path, ec) || !std::filesystem::is_regular_file(cfg.video_path, ec)) {
        res.status = 404;
        nlohmann::json err = {{"error", "Video file not found: " + cfg.video_path}};
        res.set_content(err.dump(), "application/json; charset=utf-8");
        return;
      }

      auto job = job_manager->create_job(cfg);
      res.status = 201;
      nlohmann::json resp = {
          {"job_id", job->job_id},
          {"status", to_string(job->status)}
      };
      res.set_content(resp.dump(), "application/json; charset=utf-8");
    } catch (const std::exception& e) {
      res.status = 400;
      nlohmann::json err = {{"error", std::string("Malformed JSON: ") + e.what()}};
      res.set_content(err.dump(), "application/json; charset=utf-8");
    }
  });

  // GET /api/jobs/:id/events (SSE Realtime stream)
  server.Get(R"(/api/jobs/([^/]+)/events)", [job_manager](const httplib::Request& req, httplib::Response& res) {
    if (!job_manager) {
      res.status = 500;
      return;
    }

    std::string job_id = req.matches[1];
    auto job = job_manager->get_job(job_id);
    if (!job) {
      res.status = 404;
      nlohmann::json err = {{"error", "Job not found: " + job_id}};
      res.set_content(err.dump(), "application/json; charset=utf-8");
      return;
    }

    res.set_header("Content-Type", "text/event-stream; charset=utf-8");
    res.set_header("Cache-Control", "no-cache, no-transform");
    res.set_header("Connection", "keep-alive");
    res.set_header("X-Accel-Buffering", "no");

    auto last_seq_ptr = std::make_shared<std::uint64_t>(0);

    res.set_chunked_content_provider(
        "text/event-stream; charset=utf-8",
        [job, last_seq_ptr](size_t /*offset*/, httplib::DataSink& sink) -> bool {
          if (!sink.is_writable()) {
            return false;
          }

          std::vector<JobEvent> events;
          bool has_more = job->event_stream->fetch_after(
              *last_seq_ptr, events, job->is_terminal(), std::chrono::milliseconds(2000));

          if (!events.empty()) {
            for (const auto& ev : events) {
              std::string sse_chunk = ev.to_sse_string(job->job_id);
              if (!sink.write(sse_chunk.data(), sse_chunk.size())) {
                return false;
              }
              *last_seq_ptr = ev.seq_id;
            }
          } else if (sink.is_writable()) {
            const char* ping_str = ": ping\n\n";
            if (!sink.write(ping_str, std::strlen(ping_str))) {
              return false;
            }
          }

          if (!has_more && job->is_terminal()) {
            sink.done();
            return false;
          }

          return true;
        });
  });

  // POST /api/jobs/:id/cancel (Cancel job)
  server.Post(R"(/api/jobs/([^/]+)/cancel)", [job_manager](const httplib::Request& req, httplib::Response& res) {
    if (!job_manager) {
      res.status = 500;
      return;
    }

    std::string job_id = req.matches[1];
    auto job = job_manager->get_job(job_id);
    if (!job) {
      res.status = 404;
      nlohmann::json err = {{"error", "Job not found: " + job_id}};
      res.set_content(err.dump(), "application/json; charset=utf-8");
      return;
    }

    job_manager->cancel_job(job_id);
    nlohmann::json resp = {
        {"job_id", job_id},
        {"status", "cancelled"}
    };
    res.set_content(resp.dump(), "application/json; charset=utf-8");
  });

  // GET /api/jobs/:id/export (Export SRT)
  server.Get(R"(/api/jobs/([^/]+)/export)", [job_manager](const httplib::Request& req, httplib::Response& res) {
    if (!job_manager) {
      res.status = 500;
      return;
    }

    std::string job_id = req.matches[1];
    auto job = job_manager->get_job(job_id);
    if (!job) {
      res.status = 404;
      nlohmann::json err = {{"error", "Job not found: " + job_id}};
      res.set_content(err.dump(), "application/json; charset=utf-8");
      return;
    }

    std::vector<sublift::SubtitleEntry> entries_copy;
    JobStatus current_status;
    {
      std::lock_guard<std::mutex> lk(job->state_mutex);
      current_status = job->status;
      entries_copy = job->entries;
    }

    if (current_status != JobStatus::Completed) {
      res.status = 409;
      nlohmann::json err = {{"error", "Job is not completed yet (current status: " + to_string(current_status) + ")"}};
      res.set_content(err.dump(), "application/json; charset=utf-8");
      return;
    }

    std::string srt_content = format_entries_to_srt(entries_copy);

    std::filesystem::path video_p = job->config.video_path;
    std::string srt_filename = video_p.stem().string() + ".srt";

    res.set_header("Content-Disposition", "attachment; filename=\"" + srt_filename + "\"");
    res.set_content(srt_content, "text/plain; charset=utf-8");
  });

  // Mount static directory if provided
  if (!static_dir.empty()) {
    std::error_code ec;
    if (std::filesystem::exists(static_dir, ec) && std::filesystem::is_directory(static_dir, ec)) {
      server.set_mount_point("/", static_dir);

      std::filesystem::path index_path = std::filesystem::path(static_dir) / "index.html";
      if (std::filesystem::exists(index_path, ec)) {
        server.Get("/", [index_path](const httplib::Request&, httplib::Response& res) {
          std::ifstream ifs(index_path, std::ios::binary);
          if (ifs) {
            std::string content((std::istreambuf_iterator<char>(ifs)),
                                std::istreambuf_iterator<char>());
            res.set_content(content, "text/html; charset=utf-8");
          } else {
            res.status = 404;
          }
        });
      }
    }
  }
}

}  // namespace sublift::server
