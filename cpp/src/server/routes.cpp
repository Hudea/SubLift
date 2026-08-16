#include "sublift/server/routes.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <regex>
#include <string>
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

}  // namespace

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

void register_routes(httplib::Server& server, const std::string& static_dir) {
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

  // Mount static directory if provided
  if (!static_dir.empty()) {
    std::error_code ec;
    if (std::filesystem::exists(static_dir, ec) && std::filesystem::is_directory(static_dir, ec)) {
      server.set_mount_point("/", static_dir);
    }
  }
}

}  // namespace sublift::server
