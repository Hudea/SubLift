#include "sublift/server/routes.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
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
#include "sublift/server/path_sandbox.hpp"
#include "sublift/server/region_detector.hpp"
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

// ---------------------------------------------------------------------------
// 本机指纹反查（Feature 12507）：文件名 + 大小 + 首尾 4KB 原始字节比对
// ---------------------------------------------------------------------------
namespace fingerprint {

constexpr size_t kEdgeChunkBytes = 4096;

struct Query {
  std::string name_lower;  // 小写化的目标文件名（不含目录）
  std::uint64_t size;
  std::vector<unsigned char> head;
  std::vector<unsigned char> tail;
};

/// 搜索根：显式工作区（WorkspaceManager）最高优先级；
/// 其次显式沙箱根（SUBLIFT_ALLOWED_MEDIA_ROOT）；
/// 未设置时扫描当前工作目录与本机常见用户媒体目录（浅层、限时）。
std::vector<std::filesystem::path> collect_search_roots(
    const std::shared_ptr<WorkspaceManager>& wm) {
  std::vector<std::filesystem::path> roots;
  if (wm) {
    auto w_dir = wm->get_media_dir();
    if (w_dir.has_value() && !w_dir->empty()) {
      roots.push_back(*w_dir);
      return roots;
    }
  }
  if (const char* env_root = std::getenv("SUBLIFT_ALLOWED_MEDIA_ROOT"); env_root && *env_root) {
    roots.emplace_back(env_root);
    return roots;
  }
  return roots;
}

/// 读取文件首/尾各 head.size()/tail.size() 字节并与期望逐字节比对。
/// 约定：客户端对 size > 2*kEdgeChunkBytes 的文件必须提供 tail；更小的文件
/// head 已覆盖全文件（或尾部与 head 重叠），跳过 tail 校验。
bool verify_file_edges(const std::filesystem::path& p, const Query& q) {
  if (q.head.empty()) return false;
  std::ifstream f(p, std::ios::binary);
  if (!f) return false;

  std::vector<char> buf(q.head.size());
  f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
  if (static_cast<size_t>(f.gcount()) != buf.size()) return false;
  if (std::memcmp(buf.data(), q.head.data(), q.head.size()) != 0) return false;

  if (q.size > 2 * kEdgeChunkBytes) {
    if (q.tail.empty()) return false;  // 大文件必须校验尾部
    std::vector<char> tail_buf(q.tail.size());
    f.clear();
    f.seekg(-static_cast<std::streamoff>(tail_buf.size()), std::ios::end);
    f.read(tail_buf.data(), static_cast<std::streamsize>(tail_buf.size()));
    if (static_cast<size_t>(f.gcount()) != tail_buf.size()) return false;
    if (std::memcmp(tail_buf.data(), q.tail.data(), q.tail.size()) != 0) return false;
  }
  return true;
}

/// 在受控根目录内限时（8s）、限深（6 层）扫描，返回第一个通过
/// 指纹比对 + 媒体沙箱校验（可立即用于 stream/jobs）的规范路径。
std::optional<std::filesystem::path> find_by_fingerprint(
    const Query& q, const std::shared_ptr<WorkspaceManager>& wm) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
  std::optional<std::filesystem::path> allowed_root = std::nullopt;
  if (wm && wm->get_media_dir().has_value()) {
    allowed_root = wm->get_media_dir();
  }

  for (const auto& root : collect_search_roots(wm)) {
    std::error_code ec;
    if (!std::filesystem::exists(root, ec) || !std::filesystem::is_directory(root, ec)) continue;

    std::filesystem::recursive_directory_iterator it(
        root, std::filesystem::directory_options::skip_permission_denied, ec);
    if (ec) continue;
    for (const auto end = std::filesystem::recursive_directory_iterator{}; it != end;) {
      if (std::chrono::steady_clock::now() > deadline) return std::nullopt;

      if (it->is_symlink(ec)) {
        // 不跟随符号链接，避免环与逃逸
      } else if (it->is_regular_file(ec) && !ec) {
        std::string fname = it->path().filename().string();
        std::transform(fname.begin(), fname.end(), fname.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (fname == q.name_lower) {
          const std::uintmax_t fsize = it->file_size(ec);
          if (!ec && fsize == q.size && verify_file_edges(it->path(), q)) {
            // 命中后仍须通过媒体沙箱（扩展名白名单 + 可选根目录约束），
            // 保证返回的路径立刻可用于 /api/video/stream 与 POST /api/jobs。
            try {
              auto validated = resolve_and_validate_media_path(it->path().string(), allowed_root);
              return validated;
            } catch (const PathSecurityException&) {
              // 不合规的候选（如非白名单扩展名）继续扫描
            }
          }
        }
      }

      if (it->is_directory(ec) && (it.depth() >= 6 || it->path().filename() == ".sublift_cache")) {
        it.disable_recursion_pending();
      }
      it.increment(ec);
      if (ec) break;
    }
  }
  return std::nullopt;
}

}  // namespace fingerprint

void register_routes(httplib::Server& server,
                     std::shared_ptr<JobManager> job_manager,
                     const std::string& static_dir,
                     std::shared_ptr<WorkspaceManager> workspace_manager) {
  // CORS Preflight
  server.Options(R"(/api/.*)", [](const httplib::Request&, httplib::Response& res) {
    res.status = 204;
  });

  // GET /api/system/info
  server.Get("/api/system/info", [](const httplib::Request&, httplib::Response& res) {
    auto info = collect_system_info();
    res.set_content(info.to_json().dump(), "application/json; charset=utf-8");
  });

  // GET /api/config/workspace (Feature 12508)
  server.Get("/api/config/workspace", [workspace_manager](const httplib::Request&, httplib::Response& res) {
    if (workspace_manager) {
      auto info = workspace_manager->get_workspace_info();
      nlohmann::json j = {
          {"configured", info.configured},
          {"media_dir", info.media_dir},
          {"cache_dir", info.cache_dir},
          {"video_count", info.video_count},
      };
      res.set_content(j.dump(), "application/json; charset=utf-8");
    } else {
      nlohmann::json j = {
          {"configured", false},
          {"media_dir", ""},
          {"cache_dir", ""},
          {"video_count", 0},
      };
      res.set_content(j.dump(), "application/json; charset=utf-8");
    }
  });

  // GET /api/config/workspace/videos (Feature 12508: 列出工作区内的可用视频文件)
  server.Get("/api/config/workspace/videos", [workspace_manager](const httplib::Request&, httplib::Response& res) {
    if (!workspace_manager || !workspace_manager->get_media_dir().has_value()) {
      res.set_content("[]", "application/json; charset=utf-8");
      return;
    }

    auto files = workspace_manager->list_media_files();
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& item : files) {
      arr.push_back({
          {"name", item.name},
          {"path", item.path},
          {"relative_path", item.relative_path},
          {"size_bytes", item.size_bytes},
      });
    }
    res.set_content(arr.dump(), "application/json; charset=utf-8");
  });

  // POST /api/config/workspace (Feature 12508)
  server.Post("/api/config/workspace", [workspace_manager, job_manager](const httplib::Request& req, httplib::Response& res) {
    if (!workspace_manager) {
      res.status = 500;
      res.set_content(R"({"error":"WorkspaceManager not available"})", "application/json; charset=utf-8");
      return;
    }

    nlohmann::json body;
    try {
      body = nlohmann::json::parse(req.body);
    } catch (...) {
      res.status = 400;
      res.set_content(R"({"error":"Invalid JSON body"})", "application/json; charset=utf-8");
      return;
    }

    if (!body.contains("media_dir") || !body["media_dir"].is_string()) {
      res.status = 400;
      res.set_content(R"({"error":"Missing or invalid 'media_dir' field"})", "application/json; charset=utf-8");
      return;
    }

    std::string raw_media_dir = body["media_dir"].get<std::string>();
    std::string err_msg;
    if (!workspace_manager->set_media_directory(raw_media_dir, &err_msg)) {
      res.status = 400;
      nlohmann::json err = {{"error", err_msg.empty() ? "Invalid media directory" : err_msg}};
      res.set_content(err.dump(), "application/json; charset=utf-8");
      return;
    }

    if (job_manager) {
      job_manager->set_state_file_path(workspace_manager->get_cache_dir() / "jobs_state.v1.json");
    }

    auto info = workspace_manager->get_workspace_info();
    nlohmann::json j = {
        {"configured", info.configured},
        {"media_dir", info.media_dir},
        {"cache_dir", info.cache_dir},
        {"video_count", info.video_count},
    };
    res.set_content(j.dump(), "application/json; charset=utf-8");
  });

  // Clear workspace
  auto handle_clear_workspace = [workspace_manager, job_manager](const httplib::Request&, httplib::Response& res) {
    if (workspace_manager) {
      workspace_manager->clear_workspace();
      if (job_manager) {
        job_manager->set_state_file_path("");
      }
      auto info = workspace_manager->get_workspace_info();
      nlohmann::json j = {
          {"configured", false},
          {"media_dir", ""},
          {"cache_dir", info.cache_dir},
          {"video_count", 0},
      };
      res.set_content(j.dump(), "application/json; charset=utf-8");
    } else {
      res.set_content(R"({"configured":false,"media_dir":"","cache_dir":"","video_count":0})", "application/json; charset=utf-8");
    }
  };
  server.Post("/api/config/workspace/clear", handle_clear_workspace);
  server.Delete("/api/config/workspace", handle_clear_workspace);

  // GET /api/video/frame?path=<video_path>&time_s=<time_s>
  server.Get("/api/video/frame", [workspace_manager](const httplib::Request& req, httplib::Response& res) {
    if (!req.has_param("path")) {
      res.status = 400;
      nlohmann::json err = {{"error", "Missing 'path' query parameter"}};
      res.set_content(err.dump(), "application/json; charset=utf-8");
      return;
    }

    std::optional<std::filesystem::path> allowed_root = std::nullopt;
    if (workspace_manager) {
      allowed_root = workspace_manager->get_media_dir();
    }

    std::filesystem::path video_path;
    try {
      video_path = resolve_and_validate_media_path(req.get_param_value("path"), allowed_root);
    } catch (const PathSecurityException& se) {
      res.status = 400;
      nlohmann::json err = {{"error", std::string("Path Security Error: ") + se.what()}};
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
  server.Get("/api/video/stream", [workspace_manager](const httplib::Request& req, httplib::Response& res) {
    if (!req.has_param("path")) {
      res.status = 400;
      nlohmann::json err = {{"error", "Missing 'path' query parameter"}};
      res.set_content(err.dump(), "application/json; charset=utf-8");
      return;
    }

    std::optional<std::filesystem::path> allowed_root = std::nullopt;
    if (workspace_manager) {
      allowed_root = workspace_manager->get_media_dir();
    }

    std::filesystem::path video_path;
    try {
      video_path = resolve_and_validate_media_path(req.get_param_value("path"), allowed_root);
    } catch (const PathSecurityException& se) {
      res.status = 400;
      nlohmann::json err = {{"error", std::string("Path Security Error: ") + se.what()}};
      res.set_content(err.dump(), "application/json; charset=utf-8");
      return;
    }

    std::filesystem::path stream_path = video_path;
    std::string ext = video_path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });

    // If container format is not native browser supported (e.g. MKV, AVI, FLV, MOV) or transcode is explicitly requested
    bool needs_remux = (ext == ".mkv" || ext == ".avi" || ext == ".flv" || ext == ".wmv" || ext == ".mov" ||
                        req.has_param("transcode") || req.has_param("remux"));
    if (needs_remux) {
      if (sublift::ffmpeg::available()) {
        try {
          std::optional<std::filesystem::path> custom_cache = std::nullopt;
          if (workspace_manager) {
            custom_cache = workspace_manager->get_remux_cache_dir();
          }
          stream_path = sublift::ffmpeg::remux_to_faststart_mp4(video_path, custom_cache);
        } catch (const std::exception&) {
          // If remux fails, fallback to direct streaming
          stream_path = video_path;
        }
      }
    }

    // 校验与 open 之间存在竞态删除窗口：file_size 带 error_code，
    // 文件消失时返回 404 而不是抛未捕获异常。
    std::error_code size_ec;
    const std::uint64_t total_size = std::filesystem::file_size(stream_path, size_ec);
    if (size_ec) {
      res.status = 404;
      nlohmann::json err = {{"error", "Video file vanished before streaming: " + stream_path.string()}};
      res.set_content(err.dump(), "application/json; charset=utf-8");
      return;
    }
    if (total_size == 0) {
      res.status = 200;
      res.set_header("Content-Length", "0");
      res.set_header("Accept-Ranges", "bytes");
      return;
    }

    const std::string mime_type = get_video_mime_type(stream_path);
    res.set_header("Accept-Ranges", "bytes");

    int fd = ::open(stream_path.c_str(), O_RDONLY);
    if (fd < 0) {
      res.status = 500;
      nlohmann::json err = {{"error", "Failed to open video stream on server"}};
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

  // ---------------------------------------------------------------------------
  // POST /api/video/resolve —— 本机指纹反查（Feature 12507）
  //
  // 纯浏览器拿不到用户本地文件的绝对路径，但文件就在同一台机器上。
  // 客户端提供 文件名 + 字节数 + 首尾各 4KB 的原始字节（hex），服务端在受控
  // 目录内查找同名同大小的文件并做字节级校验；命中即返回原路径，全程零拷贝。
  // 字节而非哈希：避免引入加密依赖，且比对强度等价（内容已由客户端提供）。
  // ---------------------------------------------------------------------------
  server.Post("/api/video/resolve", [workspace_manager](const httplib::Request& req, httplib::Response& res) {
    nlohmann::json body;
    try {
      body = nlohmann::json::parse(req.body);
    } catch (const std::exception&) {
      res.status = 400;
      res.set_content(R"({"error":"Invalid JSON body"})", "application/json; charset=utf-8");
      return;
    }

    if (!body.contains("name") || !body.contains("size") || !body.contains("head_hex") ||
        !body["name"].is_string() || !body["size"].is_number_unsigned() || !body["head_hex"].is_string()) {
      res.status = 400;
      res.set_content(R"json({"error":"Missing or invalid fingerprint fields (name/size/head_hex)"})json",
                      "application/json; charset=utf-8");
      return;
    }

    const std::string want_name = body["name"].get<std::string>();
    const std::uint64_t want_size = body["size"].get<std::uint64_t>();
    const std::string tail_hex =
        body.contains("tail_hex") && body["tail_hex"].is_string() ? body["tail_hex"].get<std::string>() : "";

    if (want_name.empty() || want_name.find('/') != std::string::npos ||
        want_name.find('\\') != std::string::npos) {
      res.status = 400;
      res.set_content(R"({"error":"Invalid file name"})", "application/json; charset=utf-8");
      return;
    }

    struct FingerprintQuery {
      std::string name_lower;
      std::uint64_t size;
      std::vector<unsigned char> head;
      std::vector<unsigned char> tail;
    };
    auto hex_to_bytes = [](const std::string& hex) -> std::optional<std::vector<unsigned char>> {      if (hex.size() % 2 != 0) return std::nullopt;
      std::vector<unsigned char> out;
      out.reserve(hex.size() / 2);
      for (size_t i = 0; i < hex.size(); i += 2) {
        auto nib = [](char c) -> int {
          if (c >= '0' && c <= '9') return c - '0';
          if (c >= 'a' && c <= 'f') return c - 'a' + 10;
          if (c >= 'A' && c <= 'F') return c - 'A' + 10;
          return -1;
        };
        int hi = nib(hex[i]);
        int lo = nib(hex[i + 1]);
        if (hi < 0 || lo < 0) return std::nullopt;
        out.push_back(static_cast<unsigned char>((hi << 4) | lo));
      }
      return out;
    };

    auto head_opt = hex_to_bytes(body["head_hex"].get<std::string>());
    auto tail_opt = hex_to_bytes(tail_hex);
    if (!head_opt || (!tail_hex.empty() && !tail_opt)) {
      res.status = 400;
      res.set_content(R"({"error":"head_hex/tail_hex must be valid hex"})", "application/json; charset=utf-8");
      return;
    }

    FingerprintQuery query;
    query.name_lower = want_name;
    std::transform(query.name_lower.begin(), query.name_lower.end(), query.name_lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    query.size = want_size;
    query.head = std::move(*head_opt);
    query.tail = tail_hex.empty() ? std::vector<unsigned char>{} : std::move(*tail_opt);

    if (query.head.size() > fingerprint::kEdgeChunkBytes ||
        query.tail.size() > fingerprint::kEdgeChunkBytes) {
      res.status = 400;
      res.set_content(R"({"error":"head/tail chunk exceeds limit"})", "application/json; charset=utf-8");
      return;
    }

    std::optional<std::filesystem::path> found =
        fingerprint::find_by_fingerprint({query.name_lower, query.size, std::move(query.head),
                                          std::move(query.tail)}, workspace_manager);

    if (!found.has_value()) {
      res.status = 404;
      nlohmann::json err = {{"error",
                             "No matching file located on server; paste the absolute path instead"}};
      res.set_content(err.dump(), "application/json; charset=utf-8");
      return;
    }

    nlohmann::json ok = {{"path", found->string()}};
    res.set_content(ok.dump(), "application/json; charset=utf-8");
  });

  // ---------------------------------------------------------------------------
  // POST /api/video/scan-path —— 服务端绝对路径 / 目录递归扫描与展开 (Feature 12510)
  //
  // 输入：{ "path": "/path/to/dir_or_file" } 或 { "paths": ["/path/1", "/path/2"] }
  // 输出：{ "accepted": [...], "skipped": N, "rejected": [...] }
  // ---------------------------------------------------------------------------
  server.Post("/api/video/scan-path", [workspace_manager](const httplib::Request& req, httplib::Response& res) {
    std::filesystem::path allowed_root;
    if (workspace_manager && workspace_manager->get_media_dir().has_value() &&
        !workspace_manager->get_media_dir()->empty()) {
      allowed_root = *workspace_manager->get_media_dir();
    } else if (const char* env_root = std::getenv("SUBLIFT_ALLOWED_MEDIA_ROOT"); env_root && *env_root) {
      allowed_root = std::filesystem::path(env_root);
    } else {
      res.status = 400;
      res.set_content(R"({"error":"Workspace is not configured"})", "application/json; charset=utf-8");
      return;
    }

    std::error_code root_ec;
    std::filesystem::path canonical_root = std::filesystem::canonical(allowed_root, root_ec);
    if (root_ec) {
      res.status = 400;
      res.set_content(R"json({"error":"Workspace root directory cannot be resolved (fail-closed)"})json",
                      "application/json; charset=utf-8");
      return;
    }

    nlohmann::json body;
    try {
      body = nlohmann::json::parse(req.body);
    } catch (...) {
      res.status = 400;
      res.set_content(R"({"error":"Invalid JSON body"})", "application/json; charset=utf-8");
      return;
    }

    std::vector<std::string> raw_inputs;
    if (body.contains("paths")) {
      if (!body["paths"].is_array()) {
        res.status = 400;
        res.set_content(R"({"error":"Field 'paths' must be an array"})", "application/json; charset=utf-8");
        return;
      }
      for (const auto& item : body["paths"]) {
        if (!item.is_string()) {
          res.status = 400;
          res.set_content(R"({"error":"Items in 'paths' must be strings"})", "application/json; charset=utf-8");
          return;
        }
        raw_inputs.push_back(item.get<std::string>());
      }
    } else if (body.contains("path")) {
      if (!body["path"].is_string()) {
        res.status = 400;
        res.set_content(R"({"error":"Field 'path' must be a string"})", "application/json; charset=utf-8");
        return;
      }
      raw_inputs.push_back(body["path"].get<std::string>());
    } else {
      res.status = 400;
      res.set_content(R"({"error":"Missing 'path' or 'paths' parameter"})", "application/json; charset=utf-8");
      return;
    }

    constexpr int kMaxDepth = 10;
    constexpr size_t kMaxFiles = 500;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(5000);

    nlohmann::json accepted = nlohmann::json::array();
    nlohmann::json rejected = nlohmann::json::array();
    int skipped_count = 0;
    std::set<std::string> seen_paths;

    auto generate_item_id = [](const std::string& p_str) -> std::string {
      auto h = std::hash<std::string>{}(p_str);
      char buf[32];
      std::snprintf(buf, sizeof(buf), "srv_%016llx", static_cast<unsigned long long>(h));
      return std::string(buf);
    };

    auto make_rejection = [](const std::string& path_or_name,
                             const std::string& kind,
                             const std::string& message = "",
                             const std::string& ext = "") -> nlohmann::json {
      nlohmann::json reason_obj = {{"kind", kind}};
      if (!message.empty()) {
        reason_obj["detail"] = message;
      }
      if (!ext.empty()) {
        reason_obj["extension"] = ext;
      }
      return nlohmann::json{
          {"path", path_or_name},
          {"pathOrName", path_or_name},
          {"reason", reason_obj},
          {"message", message.empty() ? kind : message}
      };
    };

    auto is_companion_subtitle_extension = [](const std::filesystem::path& p) -> bool {
      std::string ext = p.extension().string();
      std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
      });
      return (ext == ".srt" || ext == ".vtt" || ext == ".ass" || ext == ".ssa" ||
              ext == ".sub" || ext == ".sbv" || ext == ".lrc" || ext == ".idx");
    };

    for (const auto& raw_input : raw_inputs) {
      if (raw_input.empty()) continue;

      if (raw_input.find('\0') != std::string::npos) {
        rejected.push_back(make_rejection(raw_input, "unreadable", "非法路径包含空字符"));
        continue;
      }

      if (std::chrono::steady_clock::now() > deadline) {
        skipped_count++;
        break;
      }

      std::filesystem::path input_p = expand_tilde(raw_input);
      if (input_p.is_relative()) {
        input_p = canonical_root / input_p;
      }

      std::error_code ec;
      std::filesystem::path canonical_p = std::filesystem::canonical(input_p, ec);
      if (ec) {
        rejected.push_back(make_rejection(raw_input, "unreadable", "路径不存在或无法访问"));
        continue;
      }

      // Sandbox containment validation against canonical workspace root
      auto [root_end, _] = std::mismatch(
          canonical_root.begin(), canonical_root.end(),
          canonical_p.begin(), canonical_p.end());
      if (root_end != canonical_root.end()) {
        rejected.push_back(make_rejection(raw_input, "unreadable", "安全策略限制：禁止扫描工作区外部路径"));
        continue;
      }

      if (std::filesystem::is_directory(canonical_p, ec)) {
        int dir_accepted_count = 0;
        std::filesystem::recursive_directory_iterator it(
            canonical_p, std::filesystem::directory_options::skip_permission_denied, ec);
        if (ec) {
          rejected.push_back(make_rejection(raw_input, "unreadable", "目录无法访问或权限不足"));
          continue;
        }

        for (const auto end = std::filesystem::recursive_directory_iterator{}; it != end;) {
          if (std::chrono::steady_clock::now() > deadline) {
            skipped_count++;
            break;
          }

          if (accepted.size() >= kMaxFiles) {
            skipped_count++;
            if (it->is_directory(ec)) {
              it.disable_recursion_pending();
            }
            it.increment(ec);
            if (ec) break;
            continue;
          }

          if (it->is_symlink(ec)) {
            skipped_count++;
          } else if (it->is_directory(ec)) {
            std::string dir_name = it->path().filename().string();
            if (dir_name.rfind(".", 0) == 0 || dir_name == ".git" || dir_name == ".sublift_cache") {
              skipped_count++;
              it.disable_recursion_pending();
            } else if (it.depth() >= kMaxDepth) {
              it.disable_recursion_pending();
            }
          } else if (it->is_regular_file(ec) && !ec) {
            std::string filename = it->path().filename().string();
            if (filename.rfind(".", 0) == 0 || filename == ".DS_Store") {
              skipped_count++;
            } else if (is_allowed_media_extension(it->path())) {
              std::string rel = std::filesystem::relative(it->path(), canonical_root, ec).string();
              bool hidden_in_rel = false;
              for (const auto& part : std::filesystem::path(rel)) {
                std::string s = part.string();
                if (s != "." && s != ".." && s.rfind(".", 0) == 0) {
                  hidden_in_rel = true;
                  break;
                }
              }

              if (hidden_in_rel) {
                skipped_count++;
              } else {
                std::string p_str = it->path().string();
                if (seen_paths.count(p_str)) {
                  rejected.push_back(make_rejection(filename, "duplicate", "重复的文件"));
                } else {
                  seen_paths.insert(p_str);
                  std::string import_root = "";
                  if (rel.find('/') != std::string::npos) {
                    import_root = rel.substr(0, rel.rfind('/') + 1);
                  }
                  std::string ext = it->path().extension().string();
                  if (!ext.empty() && ext[0] == '.') ext = ext.substr(1);
                  std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                  });

                  auto srt_path = std::filesystem::path(it->path()).replace_extension(".srt");
                  bool output_exists = std::filesystem::exists(srt_path, ec);

                  accepted.push_back({
                      {"id", generate_item_id(p_str)},
                      {"name", filename},
                      {"videoPath", p_str},
                      {"relativePath", rel},
                      {"importRootPath", import_root},
                      {"sizeBytes", it->file_size(ec)},
                      {"format", ext},
                      {"location", "server-path"},
                      {"outputExists", output_exists}
                  });
                  dir_accepted_count++;
                }
              }
            } else if (is_companion_subtitle_extension(it->path())) {
              // Companion subtitle file; ignore without counting as skipped
            } else {
              skipped_count++;
            }
          } else {
            skipped_count++;
          }

          it.increment(ec);
          if (ec) break;
        }

        if (dir_accepted_count == 0) {
          rejected.push_back(make_rejection(raw_input, "emptyDirectory", "目录内未找到支持的媒体视频文件"));
        }
      } else if (std::filesystem::is_regular_file(canonical_p, ec)) {
        if (is_allowed_media_extension(canonical_p)) {
          std::string p_str = canonical_p.string();
          if (seen_paths.count(p_str)) {
            rejected.push_back(make_rejection(canonical_p.filename().string(), "duplicate", "重复的文件路径"));
          } else {
            if (accepted.size() >= kMaxFiles) {
              skipped_count++;
            } else {
              seen_paths.insert(p_str);
              std::string rel = std::filesystem::relative(canonical_p, canonical_root, ec).string();
              std::string import_root = "";
              if (rel.find('/') != std::string::npos) {
                import_root = rel.substr(0, rel.rfind('/') + 1);
              }
              std::string ext = canonical_p.extension().string();
              if (!ext.empty() && ext[0] == '.') ext = ext.substr(1);
              std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
              });

              auto srt_path = std::filesystem::path(canonical_p).replace_extension(".srt");
              bool output_exists = std::filesystem::exists(srt_path, ec);

              accepted.push_back({
                  {"id", generate_item_id(p_str)},
                  {"name", canonical_p.filename().string()},
                  {"videoPath", p_str},
                  {"relativePath", rel},
                  {"importRootPath", import_root},
                  {"sizeBytes", std::filesystem::file_size(canonical_p, ec)},
                  {"format", ext},
                  {"location", "server-path"},
                  {"outputExists", output_exists}
              });
            }
          }
        } else {
          rejected.push_back(make_rejection(raw_input, "unsupportedFormat", "不支持的文件格式", canonical_p.extension().string()));
        }
      } else {
        rejected.push_back(make_rejection(raw_input, "unreadable", "路径既不是常规文件也不是文件夹"));
      }
    }

    nlohmann::json res_json = {
        {"accepted", accepted},
        {"skipped", skipped_count},
        {"rejected", rejected}
    };
    res.set_content(res_json.dump(), "application/json; charset=utf-8");
  });

  // ---------------------------------------------------------------------------
  // POST /api/video/detect-region —— 智能字幕区域自动识别 (Feature 12509)
  //
  // 输入：video_path (必填), time_s (可选), engine (可选)
  // 输出：{ "detected": true, "sample_time_s": 12.5, "suggested_box": { "x": 0, "y": 0.78, "width": 1.0, "height": 0.16 }, "preview_text": "...", "confidence": 0.95, "total_candidates": 2 }
  // ---------------------------------------------------------------------------
  auto region_detector = std::make_shared<RegionDetector>();

  server.Post("/api/video/detect-region", [region_detector, workspace_manager](const httplib::Request& req, httplib::Response& res) {
    nlohmann::json body;
    try {
      body = nlohmann::json::parse(req.body);
    } catch (const std::exception&) {
      res.status = 400;
      res.set_content(R"({"error":"Invalid JSON body"})", "application/json; charset=utf-8");
      return;
    }

    if (!body.contains("video_path") || !body["video_path"].is_string() ||
        body["video_path"].get<std::string>().empty()) {
      res.status = 400;
      res.set_content(R"({"error":"Missing or invalid 'video_path' field"})", "application/json; charset=utf-8");
      return;
    }

    std::optional<std::filesystem::path> allowed_root = std::nullopt;
    if (workspace_manager) {
      allowed_root = workspace_manager->get_media_dir();
    }

    std::filesystem::path video_path;
    try {
      video_path = resolve_and_validate_media_path(body["video_path"].get<std::string>(), allowed_root);
    } catch (const PathSecurityException& se) {
      res.status = 400;
      nlohmann::json err = {{"error", std::string("Path Security Error: ") + se.what()}};
      res.set_content(err.dump(), "application/json; charset=utf-8");
      return;
    }

    std::optional<double> time_s = std::nullopt;
    if (body.contains("time_s") && body["time_s"].is_number()) {
      time_s = body["time_s"].get<double>();
    }

    std::optional<std::string> engine_pref = std::nullopt;
    if (body.contains("engine") && body["engine"].is_string()) {
      engine_pref = body["engine"].get<std::string>();
    }

    try {
      RegionDetectionResult result = region_detector->detect_region(video_path, time_s, engine_pref);
      nlohmann::json out = {
          {"detected", result.detected},
          {"sample_time_s", result.sample_time_s},
          {"suggested_box", {
              {"x", result.suggested_box.x},
              {"y", result.suggested_box.y},
              {"width", result.suggested_box.width},
              {"height", result.suggested_box.height}
          }},
          {"preview_text", result.preview_text},
          {"confidence", result.confidence},
          {"total_candidates", result.total_candidates}
      };
      res.status = 200;
      res.set_content(out.dump(), "application/json; charset=utf-8");
    } catch (const std::exception& e) {
      res.status = 500;
      nlohmann::json err = {{"error", std::string("Failed to detect subtitle region: ") + e.what()}};
      res.set_content(err.dump(), "application/json; charset=utf-8");
    }
  });

  // POST /api/jobs (Create and start a subtitle extraction job)
  server.Post("/api/jobs", [job_manager, workspace_manager](const httplib::Request& req, httplib::Response& res) {
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

      std::optional<std::filesystem::path> allowed_root = std::nullopt;
      if (workspace_manager) {
        allowed_root = workspace_manager->get_media_dir();
      }

      JobConfig cfg;
      std::string raw_path = body_json["video_path"].get<std::string>();
      try {
        cfg.video_path = resolve_and_validate_media_path(raw_path, allowed_root).string();
      } catch (const PathSecurityException& se) {
        res.status = 400;
        nlohmann::json err = {{"error", std::string("Path Security Error: ") + se.what()}};
        res.set_content(err.dump(), "application/json; charset=utf-8");
        return;
      }

      if (body_json.contains("engine") && body_json["engine"].is_string()) {
        cfg.engine = body_json["engine"].get<std::string>();
      }
      if (body_json.contains("fps") && body_json["fps"].is_number()) {
        cfg.fps = body_json["fps"].get<double>();
      }
      if (body_json.contains("confidence_threshold") && body_json["confidence_threshold"].is_number()) {
        cfg.confidence_threshold = body_json["confidence_threshold"].get<double>();
      }
      if (body_json.contains("script") && body_json["script"].is_string()) {
        cfg.script = body_json["script"].get<std::string>();
      }
      if (body_json.contains("region_box") && body_json["region_box"].is_object()) {
        cfg.region_box = RegionBox::from_json(body_json["region_box"]);
      }

      // Pre-validate OCR Engine
      auto sys_info = collect_system_info();
      if (cfg.engine != "mock") {
        bool engine_available = false;
        for (const auto& eng : sys_info.engines) {
          if (eng.name == cfg.engine && eng.available) {
            engine_available = true;
            break;
          }
        }
        if (!engine_available) {
          res.status = 400;
          nlohmann::json err = {{"error", "Unsupported or unavailable OCR engine: '" + cfg.engine + "'"}};
          res.set_content(err.dump(), "application/json; charset=utf-8");
          return;
        }
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

  // GET /api/jobs (List recent jobs)
  server.Get("/api/jobs", [job_manager](const httplib::Request&, httplib::Response& res) {
    if (!job_manager) {
      res.status = 500;
      return;
    }
    auto jobs = job_manager->get_all_jobs();
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& j : jobs) {
      arr.push_back(j->to_json());
    }
    res.status = 200;
    res.set_content(arr.dump(), "application/json; charset=utf-8");
  });

  // GET /api/jobs/:id (Get job detail)
  server.Get(R"(/api/jobs/([^/]+))", [job_manager](const httplib::Request& req, httplib::Response& res) {
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
    res.status = 200;
    res.set_content(job->to_json().dump(), "application/json; charset=utf-8");
  });

  // GET /api/jobs/:id/events (SSE Realtime stream with Last-Event-ID / cursor resumption)
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

    std::uint64_t start_seq = 0;
    if (req.has_header("Last-Event-ID")) {
      try {
        start_seq = std::stoull(req.get_header_value("Last-Event-ID"));
      } catch (...) {}
    } else if (req.has_param("cursor")) {
      try {
        start_seq = std::stoull(req.get_param_value("cursor"));
      } catch (...) {}
    }

    auto last_seq_ptr = std::make_shared<std::uint64_t>(start_seq);

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
  server.Get(R"(/api/jobs/([^/]+)/export)", [job_manager, workspace_manager](const httplib::Request& req, httplib::Response& res) {
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

    std::optional<std::filesystem::path> allowed_root = std::nullopt;
    if (workspace_manager) {
      allowed_root = workspace_manager->get_media_dir();
    }

    try {
      resolve_and_validate_media_path(job->config.video_path, allowed_root);
    } catch (const PathSecurityException& se) {
      res.status = 400;
      nlohmann::json err = {{"error", std::string("Path Security Error: ") + se.what()}};
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

  // POST /api/jobs/:id/save (Atomic save to disk within workspace)
  server.Post(R"(/api/jobs/([^/]+)/save)", [job_manager, workspace_manager](const httplib::Request& req, httplib::Response& res) {
    if (!job_manager || !workspace_manager) {
      res.status = 500;
      nlohmann::json err = {{"error", "JobManager or WorkspaceManager unavailable"}};
      res.set_content(err.dump(), "application/json; charset=utf-8");
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

    std::string target_path = "";
    ConflictPolicy policy = ConflictPolicy::DeterministicRename;
    bool allow_empty = false;

    if (!req.body.empty()) {
      try {
        auto body_json = nlohmann::json::parse(req.body);
        if (body_json.contains("target_path") && body_json["target_path"].is_string()) {
          target_path = body_json["target_path"].get<std::string>();
        }
        if (body_json.contains("conflict_policy") && body_json["conflict_policy"].is_string()) {
          policy = parse_conflict_policy(body_json["conflict_policy"].get<std::string>());
        }
        if (body_json.contains("allow_empty") && body_json["allow_empty"].is_boolean()) {
          allow_empty = body_json["allow_empty"].get<bool>();
        }
      } catch (const std::exception& e) {
        res.status = 400;
        nlohmann::json err = {{"error", std::string("Malformed JSON: ") + e.what()}};
        res.set_content(err.dump(), "application/json; charset=utf-8");
        return;
      }
    }

    if (target_path.empty()) {
      std::filesystem::path video_p = job->config.video_path;
      target_path = video_p.replace_extension(".srt").string();
    }

    std::string srt_content = format_entries_to_srt(entries_copy);
    int entry_count = static_cast<int>(entries_copy.size());

    DiskSaveResult result = workspace_manager->save_subtitles_atomic(
        target_path, srt_content, policy, allow_empty, entry_count);

    if (!result.success) {
      res.status = 400;
      nlohmann::json err = {
          {"error", result.error_message},
          {"target_path", result.target_path}
      };
      res.set_content(err.dump(), "application/json; charset=utf-8");
      return;
    }

    res.status = 200;
    nlohmann::json resp = {
        {"job_id", job_id},
        {"status", result.status},
        {"target_path", result.target_path},
        {"saved_path", result.saved_path},
        {"empty_result", result.empty_result},
        {"entry_count", result.entry_count}
    };
    res.set_content(resp.dump(), "application/json; charset=utf-8");
  });

  // POST /api/export/batch-save (Batch atomic save to disk with conflict policies)
  server.Post("/api/export/batch-save", [job_manager, workspace_manager](const httplib::Request& req, httplib::Response& res) {
    if (!job_manager || !workspace_manager) {
      res.status = 500;
      nlohmann::json err = {{"error", "JobManager or WorkspaceManager unavailable"}};
      res.set_content(err.dump(), "application/json; charset=utf-8");
      return;
    }

    std::vector<std::string> job_ids;
    ConflictPolicy policy = ConflictPolicy::DeterministicRename;
    bool allow_empty = false;

    if (!req.body.empty()) {
      try {
        auto body_json = nlohmann::json::parse(req.body);
        if (body_json.contains("job_ids") && body_json["job_ids"].is_array()) {
          for (const auto& item : body_json["job_ids"]) {
            if (item.is_string()) {
              job_ids.push_back(item.get<std::string>());
            }
          }
        }
        if (body_json.contains("conflict_policy") && body_json["conflict_policy"].is_string()) {
          policy = parse_conflict_policy(body_json["conflict_policy"].get<std::string>());
        }
        if (body_json.contains("allow_empty") && body_json["allow_empty"].is_boolean()) {
          allow_empty = body_json["allow_empty"].get<bool>();
        }
      } catch (const std::exception& e) {
        res.status = 400;
        nlohmann::json err = {{"error", std::string("Malformed JSON: ") + e.what()}};
        res.set_content(err.dump(), "application/json; charset=utf-8");
        return;
      }
    }

    if (job_ids.empty()) {
      auto all_jobs = job_manager->get_all_jobs();
      for (const auto& j : all_jobs) {
        if (j->status == JobStatus::Completed) {
          job_ids.push_back(j->job_id);
        }
      }
    }

    nlohmann::json results_arr = nlohmann::json::array();
    int saved_count = 0;
    int skipped_count = 0;
    int empty_count = 0;
    int failed_count = 0;

    for (const auto& jid : job_ids) {
      auto job = job_manager->get_job(jid);
      if (!job) {
        failed_count++;
        results_arr.push_back({
            {"job_id", jid},
            {"status", "failed"},
            {"error", "Job not found"}
        });
        continue;
      }

      std::vector<sublift::SubtitleEntry> entries_copy;
      JobStatus st;
      {
        std::lock_guard<std::mutex> lk(job->state_mutex);
        st = job->status;
        entries_copy = job->entries;
      }

      if (st != JobStatus::Completed) {
        failed_count++;
        results_arr.push_back({
            {"job_id", jid},
            {"status", "failed"},
            {"error", "Job not completed (status: " + to_string(st) + ")"}
        });
        continue;
      }

      std::filesystem::path video_p = job->config.video_path;
      std::string target_path = video_p.replace_extension(".srt").string();
      std::string srt_content = format_entries_to_srt(entries_copy);
      int entry_count = static_cast<int>(entries_copy.size());

      DiskSaveResult save_res = workspace_manager->save_subtitles_atomic(
          target_path, srt_content, policy, allow_empty, entry_count);

      if (!save_res.success) {
        failed_count++;
        results_arr.push_back({
            {"job_id", jid},
            {"status", "failed"},
            {"target_path", save_res.target_path},
            {"error", save_res.error_message}
        });
      } else {
        if (save_res.status == "saved") {
          saved_count++;
        } else if (save_res.status == "skipped") {
          skipped_count++;
        } else if (save_res.status == "empty_result") {
          empty_count++;
        }

        results_arr.push_back({
            {"job_id", jid},
            {"status", save_res.status},
            {"target_path", save_res.target_path},
            {"saved_path", save_res.saved_path},
            {"empty_result", save_res.empty_result},
            {"entry_count", save_res.entry_count}
        });
      }
    }

    nlohmann::json resp = {
        {"total", static_cast<int>(job_ids.size())},
        {"saved", saved_count},
        {"skipped", skipped_count},
        {"empty_results", empty_count},
        {"failed", failed_count},
        {"results", results_arr}
    };
    res.status = 200;
    res.set_content(resp.dump(), "application/json; charset=utf-8");
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
