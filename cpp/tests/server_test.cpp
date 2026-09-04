#include <catch2/catch_test_macros.hpp>

#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <thread>
#include <vector>

#include <httplib.h>
#include <nlohmann/json.hpp>

#include "sublift/adapters/ffmpeg.hpp"
#include "sublift/server/http_server.hpp"
#include "sublift/server/path_sandbox.hpp"
#include "sublift/server/routes.hpp"

namespace {

struct TempTestFile {
  std::filesystem::path path;

  explicit TempTestFile(const std::string& filename, const std::vector<uint8_t>& data) {
    path = std::filesystem::temp_directory_path() / filename;
    std::ofstream ofs(path, std::ios::binary);
    ofs.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    ofs.close();
  }

  ~TempTestFile() {
    std::error_code ec;
    std::filesystem::remove(path, ec);
  }
};

struct TempTestDir {
  std::filesystem::path path;

  explicit TempTestDir(const std::string& dirname_prefix) {
    auto now_ns = std::chrono::steady_clock::now().time_since_epoch().count();
    path = std::filesystem::temp_directory_path() / (dirname_prefix + "_" + std::to_string(now_ns));
    std::error_code ec;
    std::filesystem::create_directories(path, ec);
  }

  ~TempTestDir() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};

}  // namespace

TEST_CASE("Server System Info and CORS Preflight", "[server][system_info]") {
  sublift::server::HttpServer server;
  int port = server.bind_to_any_port("127.0.0.1");
  REQUIRE(port > 0);

  std::thread server_thread([&]() {
    server.listen_after_bind();
  });
  server.wait_until_ready();

  httplib::Client cli("127.0.0.1", port);
  cli.set_connection_timeout(std::chrono::seconds(2));
  // Cold-disk SHA-256 of the four paddle files can take ~1.5–2s.
  cli.set_read_timeout(std::chrono::seconds(5));

  SECTION("GET /api/system/info returns valid runtime metadata") {
    auto res = cli.Get("/api/system/info");
    REQUIRE(res != nullptr);
    REQUIRE(res->status == 200);

    auto json = nlohmann::json::parse(res->body);
    REQUIRE(json["runtime"] == "cpp");
    REQUIRE(json.contains("version"));
    REQUIRE(json.contains("capabilities"));
    REQUIRE(json["capabilities"].is_array());
    REQUIRE(json.contains("engines"));
    REQUIRE(json["engines"].is_array());
    REQUIRE(json.contains("ffmpeg"));

    bool has_mock = false;
    for (const auto& eng : json["engines"]) {
      if (eng["name"] == "mock") {
        has_mock = true;
        REQUIRE(eng["available"] == true);
      }
    }
    REQUIRE(has_mock);
  }

  SECTION("OPTIONS /api/system/info handles CORS preflight") {
    auto res = cli.Options("/api/system/info");
    REQUIRE(res != nullptr);
    REQUIRE(res->status == 204);
  }

  SECTION("CORS headers are absent unless explicitly opted in") {
    auto res = cli.Get("/api/system/info");
    REQUIRE(res != nullptr);
    REQUIRE(res->status == 200);
    REQUIRE(res->get_header_value("Access-Control-Allow-Origin").empty());
  }

  server.stop();
  if (server_thread.joinable()) {
    server_thread.join();
  }
}

TEST_CASE("Server CORS opt-in emits configured origin", "[server][system_info]") {
  sublift::server::ServerConfig config;
  config.cors_origin = "*";
  sublift::server::HttpServer server(std::move(config));
  int port = server.bind_to_any_port("127.0.0.1");
  REQUIRE(port > 0);

  std::thread server_thread([&]() {
    server.listen_after_bind();
  });
  server.wait_until_ready();

  httplib::Client cli("127.0.0.1", port);
  cli.set_connection_timeout(std::chrono::seconds(2));
  // Cold-disk SHA-256 of the four paddle files can take ~1.5–2s.
  cli.set_read_timeout(std::chrono::seconds(5));

  auto res = cli.Get("/api/system/info");
  REQUIRE(res != nullptr);
  REQUIRE(res->status == 200);
  REQUIRE(res->get_header_value("Access-Control-Allow-Origin") == "*");

  auto preflight = cli.Options("/api/system/info");
  REQUIRE(preflight != nullptr);
  REQUIRE(preflight->status == 204);
  REQUIRE(preflight->get_header_value("Access-Control-Allow-Origin") == "*");

  server.stop();
  if (server_thread.joinable()) {
    server_thread.join();
  }
}

TEST_CASE("RegionBox parsing clamps to the [0,1] contract", "[server][jobs]") {
  nlohmann::json j = {{"x", 1.5}, {"y", -0.2}, {"width", 2.0}, {"height", -1.0}};
  auto box = sublift::server::RegionBox::from_json(j);
  REQUIRE(box.x == 1.0);
  REQUIRE(box.y == 0.0);
  REQUIRE(box.width == 1.0);
  REQUIRE(box.height == 0.0);
}

TEST_CASE("Unified JobConfig DTO serialization, clamping and script support", "[server][jobs]") {
  nlohmann::json full_cfg = {
      {"video_path", "/path/to/movie.mp4"},
      {"engine", "paddle"},
      {"fps", 8.0},
      {"confidence_threshold", 0.35},
      {"script", "Hans"},
      {"region_box", {{"x", 0.05}, {"y", 0.75}, {"width", 0.90}, {"height", 0.20}}}
  };

  auto cfg = sublift::server::JobConfig::from_json(full_cfg);
  REQUIRE(cfg.video_path == "/path/to/movie.mp4");
  REQUIRE(cfg.engine == "paddle");
  REQUIRE(cfg.fps == 8.0);
  REQUIRE(cfg.confidence_threshold == 0.35);
  REQUIRE(cfg.script.has_value());
  REQUIRE(cfg.script.value() == "Hans");
  REQUIRE(cfg.region_box.x == 0.05);
  REQUIRE(cfg.region_box.y == 0.75);
  REQUIRE(cfg.region_box.width == 0.90);
  REQUIRE(cfg.region_box.height == 0.20);

  auto serialized = cfg.to_json();
  REQUIRE(serialized["video_path"] == "/path/to/movie.mp4");
  REQUIRE(serialized["engine"] == "paddle");
  REQUIRE(serialized["fps"] == 8.0);
  REQUIRE(serialized["confidence_threshold"] == 0.35);
  REQUIRE(serialized["script"] == "Hans");
  REQUIRE(serialized["region_box"]["x"] == 0.05);
  REQUIRE(serialized["region_box"]["y"] == 0.75);

  // Without optional script
  nlohmann::json no_script_cfg = {
      {"video_path", "/path/to/no_script.mp4"},
      {"engine", "vision"},
      {"fps", 5.0},
      {"confidence_threshold", 0.0}
  };
  auto cfg2 = sublift::server::JobConfig::from_json(no_script_cfg);
  REQUIRE(cfg2.video_path == "/path/to/no_script.mp4");
  REQUIRE(!cfg2.script.has_value());
  REQUIRE(cfg2.region_box.x == 0.0);
  REQUIRE(cfg2.region_box.y == 0.7);
  auto serialized2 = cfg2.to_json();
  REQUIRE(!serialized2.contains("script"));

  // In-memory JobManager preservation
  sublift::server::JobManager manager(1, 10);
  auto job = manager.create_job(cfg);
  REQUIRE(job != nullptr);
  REQUIRE(job->config.script == "Hans");
  REQUIRE(job->config.fps == 8.0);
  REQUIRE(job->config.confidence_threshold == 0.35);

  auto job_json = job->to_json();
  REQUIRE(job_json["config"]["script"] == "Hans");
  REQUIRE(job_json["config"]["fps"] == 8.0);
  REQUIRE(job_json["config"]["confidence_threshold"] == 0.35);
}

TEST_CASE("Server Video Resolve Fingerprint Lookup", "[server][resolve]") {
  // 独立沙箱根目录，保证扫描范围确定且不影响其它用例
  const auto root =
      std::filesystem::temp_directory_path() /
      ("sublift_resolve_test_" + std::to_string(::getpid()) + "_" +
       std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  REQUIRE(std::filesystem::create_directories(root));

  struct EnvGuard {
    ~EnvGuard() { ::unsetenv("SUBLIFT_ALLOWED_MEDIA_ROOT"); }
  } env_guard;
  ::setenv("SUBLIFT_ALLOWED_MEDIA_ROOT", root.c_str(), 1);

  struct RootGuard {
    std::filesystem::path root;
    ~RootGuard() {
      std::error_code ec;
      std::filesystem::remove_all(root, ec);
    }
  } root_guard{root};

  // 12288 字节周期数据（周期 251，保证首尾相位不同）；.ts 孪生文件用于白名单负例
  std::vector<uint8_t> data(12288);
  for (size_t i = 0; i < data.size(); ++i) {
    data[i] = static_cast<uint8_t>(i % 251);
  }
  const auto target = root / "sublift_resolve_target.mp4";
  const auto ts_twin = root / "sublift_resolve_secret.ts";
  {
    std::ofstream(target, std::ios::binary)
        .write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    std::ofstream(ts_twin, std::ios::binary)
        .write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
  }

  auto to_hex = [](const uint8_t* p, size_t n) {
    static const char* kHex = "0123456789abcdef";
    std::string out;
    out.reserve(n * 2);
    for (size_t i = 0; i < n; ++i) {
      out.push_back(kHex[p[i] >> 4]);
      out.push_back(kHex[p[i] & 0xf]);
    }
    return out;
  };
  const std::string head_hex = to_hex(data.data(), 4096);
  const std::string tail_hex = to_hex(data.data() + data.size() - 4096, 4096);
  std::string tampered_head = head_hex;
  tampered_head[0] = tampered_head[0] == 'f' ? '0' : 'f';
  tampered_head[1] = 'f';
  std::string tampered_tail = tail_hex;
  tampered_tail[0] = tampered_tail[0] == 'f' ? '0' : 'f';
  tampered_tail[1] = 'f';

  auto body_of = [](const std::string& name, const std::string& size,
                    const std::string& head, const std::string& tail) {
    nlohmann::json j = {{"name", name}, {"size", std::stoull(size)}, {"head_hex", head}};
    if (!tail.empty()) j["tail_hex"] = tail;
    return j.dump();
  };

  sublift::server::HttpServer server;
  int port = server.bind_to_any_port("127.0.0.1");
  REQUIRE(port > 0);
  std::thread server_thread([&]() { server.listen_after_bind(); });
  server.wait_until_ready();
  httplib::Client cli("127.0.0.1", port);
  cli.set_connection_timeout(std::chrono::seconds(2));
  cli.set_read_timeout(std::chrono::seconds(10));

  auto post_resolve = [&](const std::string& body) {
    return cli.Post("/api/video/resolve", body, "application/json");
  };

  SECTION("Exact fingerprint locates the file and the path is immediately streamable") {
    auto res = post_resolve(body_of("sublift_resolve_target.mp4", "12288", head_hex, tail_hex));
    REQUIRE(res != nullptr);
    REQUIRE(res->status == 200);
    auto json = nlohmann::json::parse(res->body);
    std::error_code ec;
    REQUIRE(json["path"].get<std::string>() == std::filesystem::canonical(target, ec).string());

    auto stream = cli.Get("/api/video/stream?path=" + json["path"].get<std::string>());
    REQUIRE(stream != nullptr);
    REQUIRE(stream->status == 200);
    REQUIRE(stream->body.size() == 12288);
  }

  SECTION("Tampered head/tail/size/name all miss with 404") {
    for (const auto& body : {
             body_of("sublift_resolve_target.mp4", "12288", tampered_head, tail_hex),
             body_of("sublift_resolve_target.mp4", "12288", head_hex, tampered_tail),
             body_of("sublift_resolve_target.mp4", "999", head_hex, ""),
             body_of("totally_other_name.mp4", "12288", head_hex, tail_hex),
         }) {
      auto res = post_resolve(body);
      REQUIRE(res != nullptr);
      REQUIRE(res->status == 404);
    }
  }

  SECTION("Non-whitelisted extension twin is never returned") {
    auto res = post_resolve(body_of("sublift_resolve_secret.ts", "12288", head_hex, tail_hex));
    REQUIRE(res != nullptr);
    REQUIRE(res->status == 404);
  }

  SECTION("Invalid payloads are rejected with 400") {
    auto no_name = post_resolve(R"({"size":10,"head_hex":"00"})");
    REQUIRE(no_name != nullptr);
    REQUIRE(no_name->status == 400);
    auto bad_hex = post_resolve(body_of("x.mp4", "10", "zz", ""));
    REQUIRE(bad_hex != nullptr);
    REQUIRE(bad_hex->status == 400);
  }

  server.stop();
  if (server_thread.joinable()) {
    server_thread.join();
  }
}

TEST_CASE("Server Video Stream HTTP 206 Partial Content", "[server][video_stream]") {
  // Create a 10,000 bytes fake MP4 file
  std::vector<uint8_t> dummy_data(10000);
  for (size_t i = 0; i < dummy_data.size(); ++i) {
    dummy_data[i] = static_cast<uint8_t>(i % 256);
  }
  TempTestFile temp_video("sublift_test_video_stream.mp4", dummy_data);

  sublift::server::ServerConfig cfg;
  cfg.media_dir = std::filesystem::temp_directory_path().string();
  sublift::server::HttpServer server(std::move(cfg));
  int port = server.bind_to_any_port("127.0.0.1");
  REQUIRE(port > 0);

  std::thread server_thread([&]() {
    server.listen_after_bind();
  });
  server.wait_until_ready();

  httplib::Client cli("127.0.0.1", port);
  cli.set_connection_timeout(std::chrono::seconds(2));
  cli.set_read_timeout(std::chrono::seconds(2));

  SECTION("Missing path parameter returns 400 Bad Request") {
    auto res = cli.Get("/api/video/stream");
    REQUIRE(res != nullptr);
    REQUIRE(res->status == 400);
  }

  SECTION("Nonexistent or non-media path returns 400 Path Security Error") {
    auto res = cli.Get("/api/video/stream?path=/nonexistent/path/test.mp4");
    REQUIRE(res != nullptr);
    REQUIRE(res->status == 400);

    auto res_sec = cli.Get("/api/video/stream?path=/etc/shadow");
    REQUIRE(res_sec != nullptr);
    REQUIRE(res_sec->status == 400);
  }

  SECTION("TypeScript .ts files are rejected despite media-like extension") {
    // .ts 与源码扩展名冲突且浏览器无法原生播放，必须在白名单外（LFI 残留封堵）
    TempTestFile ts_file("sublift_test_secret.ts", {'S', 'E', 'C', 'R', 'E', 'T'});
    auto res = cli.Get("/api/video/stream?path=" + ts_file.path.string());
    REQUIRE(res != nullptr);
    REQUIRE(res->status == 400);
    REQUIRE(res->body.find("SECRET") == std::string::npos);
  }

  SECTION("Full video request returns 200 with Accept-Ranges") {
    std::string uri = "/api/video/stream?path=" + temp_video.path.string();
    auto res = cli.Get(uri.c_str());
    REQUIRE(res != nullptr);
    REQUIRE(res->status == 200);
    REQUIRE(res->get_header_value("Accept-Ranges") == "bytes");
    REQUIRE(res->get_header_value("Content-Type") == "video/mp4");
    REQUIRE(res->get_header_value("Content-Length") == "10000");
    REQUIRE(res->body.size() == 10000);
  }

  SECTION("Safari initial probe Range bytes=0-1 returns 206 Partial Content") {
    httplib::Headers headers = {{"Range", "bytes=0-1"}};
    std::string uri = "/api/video/stream?path=" + temp_video.path.string();
    auto res = cli.Get(uri.c_str(), headers);
    REQUIRE(res != nullptr);
    REQUIRE(res->status == 206);
    REQUIRE(res->get_header_value("Accept-Ranges") == "bytes");
    REQUIRE(res->get_header_value("Content-Range") == "bytes 0-1/10000");
    REQUIRE(res->get_header_value("Content-Length") == "2");
    REQUIRE(res->body.size() == 2);
    REQUIRE(static_cast<uint8_t>(res->body[0]) == dummy_data[0]);
    REQUIRE(static_cast<uint8_t>(res->body[1]) == dummy_data[1]);
  }

  SECTION("Arbitrary Range bytes=1000-1999 returns 206 and exact payload") {
    httplib::Headers headers = {{"Range", "bytes=1000-1999"}};
    std::string uri = "/api/video/stream?path=" + temp_video.path.string();
    auto res = cli.Get(uri.c_str(), headers);
    REQUIRE(res != nullptr);
    REQUIRE(res->status == 206);
    REQUIRE(res->get_header_value("Content-Range") == "bytes 1000-1999/10000");
    REQUIRE(res->get_header_value("Content-Length") == "1000");
    REQUIRE(res->body.size() == 1000);

    bool bytes_match = true;
    for (size_t i = 0; i < 1000; ++i) {
      if (static_cast<uint8_t>(res->body[i]) != dummy_data[1000 + i]) {
        bytes_match = false;
        break;
      }
    }
    REQUIRE(bytes_match);
  }

  SECTION("Open-ended Range bytes=5000- returns 206 and correct slice") {
    httplib::Headers headers = {{"Range", "bytes=5000-"}};
    std::string uri = "/api/video/stream?path=" + temp_video.path.string();
    auto res = cli.Get(uri.c_str(), headers);
    REQUIRE(res != nullptr);
    REQUIRE(res->status == 206);
    REQUIRE(res->get_header_value("Content-Range") == "bytes 5000-9999/10000");
    REQUIRE(res->get_header_value("Content-Length") == "5000");
    REQUIRE(res->body.size() == 5000);
    REQUIRE(static_cast<uint8_t>(res->body[0]) == dummy_data[5000]);
    REQUIRE(static_cast<uint8_t>(res->body.back()) == dummy_data[9999]);
  }

  SECTION("Suffix Range bytes=-500 returns 206 and last 500 bytes") {
    httplib::Headers headers = {{"Range", "bytes=-500"}};
    std::string uri = "/api/video/stream?path=" + temp_video.path.string();
    auto res = cli.Get(uri.c_str(), headers);
    REQUIRE(res != nullptr);
    REQUIRE(res->status == 206);
    REQUIRE(res->get_header_value("Content-Range") == "bytes 9500-9999/10000");
    REQUIRE(res->get_header_value("Content-Length") == "500");
    REQUIRE(res->body.size() == 500);
    REQUIRE(static_cast<uint8_t>(res->body[0]) == dummy_data[9500]);
    REQUIRE(static_cast<uint8_t>(res->body.back()) == dummy_data[9999]);
  }

  SECTION("Out-of-bounds Range returns 416 Range Not Satisfiable") {
    httplib::Headers headers = {{"Range", "bytes=20000-30000"}};
    std::string uri = "/api/video/stream?path=" + temp_video.path.string();
    auto res = cli.Get(uri.c_str(), headers);
    REQUIRE(res != nullptr);
    REQUIRE(res->status == 416);
  }

  SECTION("Remux non-native video container (e.g. MKV) to Faststart MP4 stream") {
    if (sublift::ffmpeg::available()) {
      std::filesystem::path test_mkv = std::filesystem::temp_directory_path() / "sublift_synth_stream_test.mkv";
      std::string ffmpeg_bin = sublift::ffmpeg::resolve_ffmpeg_bin();
      std::string cmd = ffmpeg_bin + " -y -f lavfi -i testsrc=duration=0.5:size=64x64:rate=10 -pix_fmt yuv420p " + test_mkv.string() + " > /dev/null 2>&1";
      int ret = std::system(cmd.c_str());
      if (ret == 0 && std::filesystem::exists(test_mkv)) {
        std::string uri = "/api/video/stream?path=" + test_mkv.string();
        auto res = cli.Get(uri.c_str());
        REQUIRE(res != nullptr);
        REQUIRE(res->status == 200);
        REQUIRE(res->get_header_value("Content-Type") == "video/mp4");
        REQUIRE(res->get_header_value("Accept-Ranges") == "bytes");
        REQUIRE(!res->body.empty());
        std::filesystem::remove(test_mkv);
      }
    }
  }

  SECTION("Explicit transcode=1 query forces remux/transcode to Faststart MP4") {
    if (sublift::ffmpeg::available()) {
      std::string uri = "/api/video/stream?path=" + temp_video.path.string() + "&transcode=1";
      auto res = cli.Get(uri.c_str());
      REQUIRE(res != nullptr);
      // Returns 200 or 206 for stream
      REQUIRE((res->status == 200 || res->status == 206));
      REQUIRE(res->get_header_value("Content-Type") == "video/mp4");
      REQUIRE(res->get_header_value("Accept-Ranges") == "bytes");
    }
  }

  server.stop();
  if (server_thread.joinable()) {
    server_thread.join();
  }
}

TEST_CASE("Server Video Frame Extraction", "[server][video_frame]") {
  sublift::server::ServerConfig cfg;
  cfg.media_dir = std::filesystem::temp_directory_path().string();
  sublift::server::HttpServer server(std::move(cfg));
  int port = server.bind_to_any_port("127.0.0.1");
  REQUIRE(port > 0);

  std::thread server_thread([&]() {
    server.listen_after_bind();
  });
  server.wait_until_ready();

  httplib::Client cli("127.0.0.1", port);
  cli.set_connection_timeout(std::chrono::seconds(2));
  cli.set_read_timeout(std::chrono::seconds(5));

  SECTION("Missing path parameter returns 400 Bad Request") {
    auto res = cli.Get("/api/video/frame");
    REQUIRE(res != nullptr);
    REQUIRE(res->status == 400);
  }

  SECTION("Nonexistent or non-media path returns 400 Path Security Error") {
    auto res = cli.Get("/api/video/frame?path=/nonexistent/dummy.mp4");
    REQUIRE(res != nullptr);
    REQUIRE(res->status == 400);

    auto res_sec = cli.Get("/api/video/frame?path=/etc/passwd");
    REQUIRE(res_sec != nullptr);
    REQUIRE(res_sec->status == 400);
  }

  if (sublift::ffmpeg::available()) {
    // Generate a temporary 0.5s synthetic mp4
    std::filesystem::path test_mp4 = std::filesystem::temp_directory_path() / "sublift_synth_frame_test.mp4";
    std::string ffmpeg_bin = sublift::ffmpeg::resolve_ffmpeg_bin();
    std::string cmd = ffmpeg_bin + " -y -f lavfi -i testsrc=duration=0.5:size=64x64:rate=10 -pix_fmt yuv420p " + test_mp4.string() + " > /dev/null 2>&1";
    int ret = std::system(cmd.c_str());
    if (ret == 0 && std::filesystem::exists(test_mp4)) {
      SECTION("Extract frame returns valid JPEG header (SOI+EOI) and 200 OK") {
        std::string uri = "/api/video/frame?path=" + test_mp4.string() + "&time_s=0.1";
        auto res = cli.Get(uri.c_str());
        REQUIRE(res != nullptr);
        REQUIRE(res->status == 200);
        REQUIRE(res->get_header_value("Content-Type") == "image/jpeg");
        REQUIRE(res->body.size() > 4);
        // JPEG SOI marker: 0xFF, 0xD8
        REQUIRE(static_cast<uint8_t>(res->body[0]) == 0xFF);
        REQUIRE(static_cast<uint8_t>(res->body[1]) == 0xD8);
        // JPEG EOI marker: 0xFF, 0xD9
        size_t len = res->body.size();
        REQUIRE(static_cast<uint8_t>(res->body[len - 2]) == 0xFF);
        REQUIRE(static_cast<uint8_t>(res->body[len - 1]) == 0xD9);
      }

      SECTION("Negative time_s clamps to 0.0s and returns 200 OK") {
        std::string uri = "/api/video/frame?path=" + test_mp4.string() + "&time_s=-2.5";
        auto res = cli.Get(uri.c_str());
        REQUIRE(res != nullptr);
        REQUIRE(res->status == 200);
        REQUIRE(res->get_header_value("Content-Type") == "image/jpeg");
        REQUIRE(static_cast<uint8_t>(res->body[0]) == 0xFF);
        REQUIRE(static_cast<uint8_t>(res->body[1]) == 0xD8);
      }

      std::error_code ec;
      std::filesystem::remove(test_mp4, ec);
    }
  }

  server.stop();
  if (server_thread.joinable()) {
    server_thread.join();
  }
}

TEST_CASE("SRT Time and Formatting Utilities", "[server][srt]") {
  SECTION("format_srt_timestamp converts milliseconds correctly") {
    REQUIRE(sublift::server::format_srt_timestamp(0) == "00:00:00,000");
    REQUIRE(sublift::server::format_srt_timestamp(1500) == "00:00:01,500");
    REQUIRE(sublift::server::format_srt_timestamp(65432) == "00:01:05,432");
    REQUIRE(sublift::server::format_srt_timestamp(3661005) == "01:01:01,005");
    REQUIRE(sublift::server::format_srt_timestamp(-500) == "00:00:00,000");
  }

  SECTION("format_entries_to_srt produces valid standard SRT structure") {
    std::vector<sublift::SubtitleEntry> entries = {
        {.start_ms = 1000, .end_ms = 3000, .text = "Hello world", .confidence = 0.95},
        {.start_ms = 3500, .end_ms = 5000, .text = "Second line", .confidence = 0.98},
    };

    std::string srt = sublift::server::format_entries_to_srt(entries);
    std::string expected =
        "1\n"
        "00:00:01,000 --> 00:00:03,000\n"
        "Hello world\n\n"
        "2\n"
        "00:00:03,500 --> 00:00:05,000\n"
        "Second line\n\n";

    REQUIRE(srt == expected);
  }

  SECTION("format_entries_to_srt skips blank text entries") {
    std::vector<sublift::SubtitleEntry> entries = {
        {.start_ms = 1000, .end_ms = 3000, .text = "   \n\t", .confidence = 0.5},
        {.start_ms = 4000, .end_ms = 6000, .text = "Valid text", .confidence = 0.99},
    };

    std::string srt = sublift::server::format_entries_to_srt(entries);
    std::string expected =
        "1\n"
        "00:00:04,000 --> 00:00:06,000\n"
        "Valid text\n\n";

    REQUIRE(srt == expected);
  }
}

TEST_CASE("Server Job Management, SSE and Export", "[server][jobs]") {
  sublift::server::ServerConfig cfg;
  cfg.media_dir = std::filesystem::temp_directory_path().string();
  sublift::server::HttpServer server(std::move(cfg));
  int port = server.bind_to_any_port("127.0.0.1");
  REQUIRE(port > 0);

  std::thread server_thread([&]() {
    server.listen_after_bind();
  });
  server.wait_until_ready();

  httplib::Client cli("127.0.0.1", port);
  cli.set_connection_timeout(std::chrono::seconds(2));
  cli.set_read_timeout(std::chrono::seconds(5));

  SECTION("POST /api/jobs validates request body") {
    // Missing body
    auto res1 = cli.Post("/api/jobs", "invalid json", "application/json");
    REQUIRE(res1 != nullptr);
    REQUIRE(res1->status == 400);

    // Missing video_path
    auto res2 = cli.Post("/api/jobs", "{}", "application/json");
    REQUIRE(res2 != nullptr);
    REQUIRE(res2->status == 400);

    // Non-existent video file (fails path sandbox check)
    nlohmann::json req_nonexist = {{"video_path", "/nonexistent/video.mp4"}};
    auto res3 = cli.Post("/api/jobs", req_nonexist.dump(), "application/json");
    REQUIRE(res3 != nullptr);
    REQUIRE(res3->status == 400);

    // Forbidden non-media file (LFI protection)
    nlohmann::json req_lfi = {{"video_path", "/etc/hosts"}};
    auto res_lfi = cli.Post("/api/jobs", req_lfi.dump(), "application/json");
    REQUIRE(res_lfi != nullptr);
    REQUIRE(res_lfi->status == 400);

    // Create a temporary valid media file for valid path checks
    TempTestFile temp_job_video("sublift_synth_job_dummy.mp4", {0x00, 0x00, 0x00, 0x18, 'f', 't', 'y', 'p'});

    // Unsupported engine
    nlohmann::json req_bad_engine = {
        {"video_path", temp_job_video.path.string()},
        {"engine", "unsupported_fake_ocr_engine"}
    };
    auto res_bad_eng = cli.Post("/api/jobs", req_bad_engine.dump(), "application/json");
    REQUIRE(res_bad_eng != nullptr);
    REQUIRE(res_bad_eng->status == 400);
  }

  SECTION("POST /api/jobs/:id/cancel returns 404 for unknown job") {
    auto res = cli.Post("/api/jobs/unknown-uuid-12345/cancel", "{}", "application/json");
    REQUIRE(res != nullptr);
    REQUIRE(res->status == 404);
  }

  SECTION("GET /api/jobs/:id/export returns 404 for unknown job") {
    auto res = cli.Get("/api/jobs/unknown-uuid-12345/export");
    REQUIRE(res != nullptr);
    REQUIRE(res->status == 404);
  }

  SECTION("GET /api/jobs/:id/events returns 404 for unknown job") {
    auto res = cli.Get("/api/jobs/unknown-uuid-12345/events");
    REQUIRE(res != nullptr);
    REQUIRE(res->status == 404);
  }

  if (sublift::ffmpeg::available()) {
    // Generate a temporary 0.4s synthetic video for end-to-end extraction with mock engine
    std::filesystem::path test_mp4 = std::filesystem::temp_directory_path() / "sublift_synth_job_test.mp4";
    std::string ffmpeg_bin = sublift::ffmpeg::resolve_ffmpeg_bin();
    std::string cmd = ffmpeg_bin + " -y -f lavfi -i testsrc=duration=0.4:size=128x128:rate=10 -pix_fmt yuv420p " + test_mp4.string() + " > /dev/null 2>&1";
    int ret = std::system(cmd.c_str());

    if (ret == 0 && std::filesystem::exists(test_mp4)) {
      nlohmann::json job_req = {
          {"video_path", test_mp4.string()},
          {"engine", "mock"},
          {"fps", 5.0},
          {"confidence_threshold", 0.0},
          {"region_box", {{"x", 0.0}, {"y", 0.6}, {"width", 1.0}, {"height", 0.4}}}
      };

      auto res = cli.Post("/api/jobs", job_req.dump(), "application/json");
      REQUIRE(res != nullptr);
      REQUIRE(res->status == 201);

      auto resp_json = nlohmann::json::parse(res->body);
      REQUIRE(resp_json.contains("job_id"));
      std::string job_id = resp_json["job_id"].get<std::string>();
      REQUIRE(!job_id.empty());

      // Wait up to 5 seconds for job completion
      auto job_ctx = server.job_manager()->get_job(job_id);
      REQUIRE(job_ctx != nullptr);

      int wait_count = 0;
      while (!job_ctx->is_terminal() && wait_count < 50) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        wait_count++;
      }

      REQUIRE(job_ctx->is_terminal());
      REQUIRE(job_ctx->status == sublift::server::JobStatus::Completed);

      auto exp_res = cli.Get(("/api/jobs/" + job_id + "/export").c_str());
      REQUIRE(exp_res != nullptr);
      REQUIRE(exp_res->status == 200);
      REQUIRE(exp_res->get_header_value("Content-Type") == "text/plain; charset=utf-8");
      REQUIRE(exp_res->get_header_value("Content-Disposition").find("attachment;") != std::string::npos);
      if (!exp_res->body.empty()) {
        REQUIRE(exp_res->body.find("-->") != std::string::npos);
      }

      std::error_code ec;
      std::filesystem::remove(test_mp4, ec);
    }
  }

  server.stop();
  if (server_thread.joinable()) {
    server_thread.join();
  }
}

TEST_CASE("WorkspaceManager Core and Cache Directories", "[server][workspace]") {
  std::error_code ec;
  auto temp_dir = std::filesystem::temp_directory_path() / ("sublift_ws_test_" + std::to_string(::getpid()));
  std::filesystem::create_directories(temp_dir, ec);
  auto config_file = temp_dir / "workspace_cfg.json";

  // Create a dummy video file in temp_dir
  auto dummy_vid = temp_dir / "clip1.mp4";
  {
    std::ofstream f(dummy_vid);
    f << "dummy mp4 content";
  }

  SECTION("Initial unconfigured state") {
    sublift::server::WorkspaceManager wm("", config_file.string());
    auto info = wm.get_workspace_info();
    REQUIRE_FALSE(info.configured);
    REQUIRE(info.media_dir.empty());
    REQUIRE_FALSE(wm.get_media_dir().has_value());
  }

  SECTION("Configure valid directory creates .sublift_cache and counts videos") {
    sublift::server::WorkspaceManager wm("", config_file.string());
    std::string err;
    bool ok = wm.set_media_directory(temp_dir.string(), &err);
    REQUIRE(ok);
    REQUIRE(err.empty());

    auto info = wm.get_workspace_info();
    REQUIRE(info.configured);
    REQUIRE(info.media_dir == std::filesystem::canonical(temp_dir).string());
    REQUIRE(info.video_count == 1);
    REQUIRE(info.cache_dir == (std::filesystem::canonical(temp_dir) / ".sublift_cache").string());

    // Verify list_media_files returns dummy_vid
    auto files = wm.list_media_files();
    REQUIRE(files.size() == 1);
    REQUIRE(files[0].name == "clip1.mp4");
    REQUIRE(files[0].path == std::filesystem::canonical(dummy_vid).string());
    REQUIRE(files[0].relative_path == "clip1.mp4");
    REQUIRE(files[0].size_bytes > 0);
  }

  SECTION("Invalid directory rejected with error message") {
    sublift::server::WorkspaceManager wm("", config_file.string());
    std::string err;
    bool ok = wm.set_media_directory("/nonexistent_path_xyz_12345", &err);
    REQUIRE_FALSE(ok);
    REQUIRE_FALSE(err.empty());

    // File instead of directory
    ok = wm.set_media_directory(dummy_vid.string(), &err);
    REQUIRE_FALSE(ok);
    REQUIRE(err.find("不是文件夹目录") != std::string::npos);
  }

  SECTION("Persistence across instances") {
    {
      sublift::server::WorkspaceManager wm1("", config_file.string());
      wm1.set_media_directory(temp_dir.string());
    }
    {
      // New instance loading same config file
      sublift::server::WorkspaceManager wm2("", config_file.string());
      auto info = wm2.get_workspace_info();
      REQUIRE(info.configured);
      REQUIRE(info.media_dir == std::filesystem::canonical(temp_dir).string());
    }
    {
      // Clear workspace
      sublift::server::WorkspaceManager wm3("", config_file.string());
      wm3.clear_workspace();
      REQUIRE_FALSE(wm3.get_workspace_info().configured);
    }
  }

  std::filesystem::remove_all(temp_dir, ec);
}

TEST_CASE("Workspace Configuration API Endpoints", "[server][workspace_api]") {
  std::error_code ec;
  auto temp_dir = std::filesystem::temp_directory_path() / ("sublift_api_ws_" + std::to_string(::getpid()));
  std::filesystem::create_directories(temp_dir, ec);
  auto config_file = temp_dir / "cfg.json";

  // Create a dummy mp4
  auto dummy_file = temp_dir / "sample.mp4";
  {
    std::ofstream ofs(dummy_file);
    ofs << "sample video bytes";
  }

  sublift::server::ServerConfig cfg;
  cfg.port = 0;
  cfg.cors_origin = "*";
  cfg.config_file = config_file.string();

  sublift::server::HttpServer server(std::move(cfg));
  int port = server.bind_to_any_port("127.0.0.1");
  REQUIRE(port > 0);

  std::thread server_thread([&server]() {
    server.listen_after_bind();
  });

  httplib::Client cli("127.0.0.1", port);
  cli.set_connection_timeout(2, 0);
  cli.set_read_timeout(5, 0);

  // 1. Initial GET /api/config/workspace
  auto res1 = cli.Get("/api/config/workspace");
  REQUIRE(res1 != nullptr);
  REQUIRE(res1->status == 200);
  auto j1 = nlohmann::json::parse(res1->body);
  REQUIRE_FALSE(j1["configured"].get<bool>());
  REQUIRE(j1["media_dir"].get<std::string>().empty());

  // 2. Invalid POST /api/config/workspace
  auto err_res = cli.Post("/api/config/workspace", R"({"media_dir":"/invalid_nonexistent_xyz"})", "application/json");
  REQUIRE(err_res != nullptr);
  REQUIRE(err_res->status == 400);

  // 3. Valid POST /api/config/workspace
  nlohmann::json set_req = {{"media_dir", temp_dir.string()}};
  auto ok_res = cli.Post("/api/config/workspace", set_req.dump(), "application/json");
  REQUIRE(ok_res != nullptr);
  REQUIRE(ok_res->status == 200);
  auto ok_j = nlohmann::json::parse(ok_res->body);
  REQUIRE(ok_j["configured"].get<bool>());
  REQUIRE(ok_j["media_dir"].get<std::string>() == std::filesystem::canonical(temp_dir).string());

  // 4. GET /api/config/workspace/videos returns sample.mp4
  auto vid_res = cli.Get("/api/config/workspace/videos");
  REQUIRE(vid_res != nullptr);
  REQUIRE(vid_res->status == 200);
  auto vids_json = nlohmann::json::parse(vid_res->body);
  REQUIRE(vids_json.is_array());
  REQUIRE(vids_json.size() == 1);
  REQUIRE(vids_json[0]["name"].get<std::string>() == "sample.mp4");

  // 5. GET /api/config/workspace confirms configured state
  auto res2 = cli.Get("/api/config/workspace");
  REQUIRE(res2 != nullptr);
  REQUIRE(res2->status == 200);
  auto j2 = nlohmann::json::parse(res2->body);
  REQUIRE(j2["configured"].get<bool>());

  // 6. POST /api/config/workspace/clear
  auto clr_res = cli.Post("/api/config/workspace/clear", "", "application/json");
  REQUIRE(clr_res != nullptr);
  REQUIRE(clr_res->status == 200);
  auto clr_j = nlohmann::json::parse(clr_res->body);
  REQUIRE_FALSE(clr_j["configured"].get<bool>());

  server.stop();
  if (server_thread.joinable()) {
    server_thread.join();
  }

  std::filesystem::remove_all(temp_dir, ec);
}

TEST_CASE("Server Auto ROI Detection", "[server][detect_region]") {
  sublift::server::ServerConfig cfg;
  cfg.media_dir = std::filesystem::temp_directory_path().string();
  sublift::server::HttpServer server(std::move(cfg));
  int port = server.bind_to_any_port("127.0.0.1");
  REQUIRE(port > 0);

  std::thread server_thread([&]() {
    server.listen_after_bind();
  });

  httplib::Client cli("127.0.0.1", port);
  cli.set_connection_timeout(5, 0);
  cli.set_read_timeout(10, 0);

  SECTION("Missing or invalid video_path returns 400") {
    auto res1 = cli.Post("/api/video/detect-region", R"({})", "application/json");
    REQUIRE(res1 != nullptr);
    REQUIRE(res1->status == 400);

    auto res2 = cli.Post("/api/video/detect-region", R"({"video_path":""})", "application/json");
    REQUIRE(res2 != nullptr);
    REQUIRE(res2->status == 400);
  }

  SECTION("Nonexistent or path traversal is rejected with 400") {
    auto res = cli.Post("/api/video/detect-region", R"({"video_path":"/nonexistent_xyz_123.mp4"})", "application/json");
    REQUIRE(res != nullptr);
    REQUIRE(res->status == 400);

    auto res_lfi = cli.Post("/api/video/detect-region", R"({"video_path":"/etc/passwd"})", "application/json");
    REQUIRE(res_lfi != nullptr);
    REQUIRE(res_lfi->status == 400);
  }

  SECTION("Detect region on valid video returns structured suggestion") {
    if (sublift::ffmpeg::available()) {
      std::filesystem::path test_video = std::filesystem::temp_directory_path() / "sublift_roi_detect_test.mp4";
      std::string ffmpeg_bin = sublift::ffmpeg::resolve_ffmpeg_bin();
      std::string cmd = ffmpeg_bin + " -y -f lavfi -i testsrc=duration=1.0:size=320x240:rate=10 -pix_fmt yuv420p " + test_video.string() + " > /dev/null 2>&1";
      int ret = std::system(cmd.c_str());
      if (ret == 0 && std::filesystem::exists(test_video)) {
        nlohmann::json req_body = {
            {"video_path", test_video.string()},
            {"engine", "mock"}
        };
        auto res = cli.Post("/api/video/detect-region", req_body.dump(), "application/json");
        REQUIRE(res != nullptr);
        REQUIRE(res->status == 200);

        auto j = nlohmann::json::parse(res->body);
        REQUIRE(j.contains("detected"));
        REQUIRE(j.contains("sample_time_s"));
        REQUIRE(j.contains("suggested_box"));
        REQUIRE(j["suggested_box"].contains("x"));
        REQUIRE(j["suggested_box"].contains("y"));
        REQUIRE(j["suggested_box"].contains("width"));
        REQUIRE(j["suggested_box"].contains("height"));

        double x = j["suggested_box"]["x"].get<double>();
        double y = j["suggested_box"]["y"].get<double>();
        double w = j["suggested_box"]["width"].get<double>();
        double h = j["suggested_box"]["height"].get<double>();

        REQUIRE(x >= 0.0);
        REQUIRE(x <= 1.0);
        REQUIRE(y >= 0.0);
        REQUIRE(y <= 1.0);
        REQUIRE(w > 0.0);
        REQUIRE(w <= 1.0);
        REQUIRE(h > 0.0);
        REQUIRE(h <= 1.0);
        REQUIRE(y + h <= 1.0001);

        // Test with explicit time_s
        nlohmann::json req_time = {
            {"video_path", test_video.string()},
            {"time_s", 0.5},
            {"engine", "mock"}
        };
        auto res_time = cli.Post("/api/video/detect-region", req_time.dump(), "application/json");
        REQUIRE(res_time != nullptr);
        REQUIRE(res_time->status == 200);
        auto j_time = nlohmann::json::parse(res_time->body);
        REQUIRE(j_time["sample_time_s"].get<double>() == 0.5);

        std::filesystem::remove(test_video);
      }
    }
  }

  server.stop();
  if (server_thread.joinable()) {
    server_thread.join();
  }
}

namespace {

struct EnvVarGuard {
  std::string name;
  std::optional<std::string> original_val;

  explicit EnvVarGuard(std::string var_name, std::optional<std::string> new_val = std::nullopt)
      : name(std::move(var_name)) {
    const char* val = std::getenv(name.c_str());
    if (val) {
      original_val = std::string(val);
    }
    if (new_val.has_value()) {
      ::setenv(name.c_str(), new_val->c_str(), 1);
    } else {
      ::unsetenv(name.c_str());
    }
  }

  ~EnvVarGuard() {
    if (original_val.has_value()) {
      ::setenv(name.c_str(), original_val->c_str(), 1);
    } else {
      ::unsetenv(name.c_str());
    }
  }
};

struct ScanServerFixture {
  std::filesystem::path temp_dir;
  std::filesystem::path canonical_temp_dir;
  std::filesystem::path config_file;
  std::unique_ptr<sublift::server::HttpServer> server;
  std::thread server_thread;
  std::unique_ptr<httplib::Client> cli;
  int port{0};

  explicit ScanServerFixture(bool configure_media_dir = true) {
    std::error_code ec;
    auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    temp_dir = std::filesystem::temp_directory_path() /
               ("sublift_scan_test_" + std::to_string(::getpid()) + "_" + std::to_string(now));
    std::filesystem::create_directories(temp_dir, ec);
    canonical_temp_dir = std::filesystem::canonical(temp_dir, ec);
    config_file = temp_dir / "cfg.json";

    sublift::server::ServerConfig cfg;
    cfg.port = 0;
    cfg.cors_origin = "*";
    if (configure_media_dir) {
      cfg.media_dir = canonical_temp_dir.string();
    }
    cfg.config_file = config_file.string();

    server = std::make_unique<sublift::server::HttpServer>(std::move(cfg));
    port = server->bind_to_any_port("127.0.0.1");
    if (port <= 0) {
      throw std::runtime_error("Failed to bind test server port");
    }

    server_thread = std::thread([this]() {
      server->listen_after_bind();
    });
    server->wait_until_ready();

    cli = std::make_unique<httplib::Client>("127.0.0.1", port);
    cli->set_connection_timeout(5, 0);
    cli->set_read_timeout(10, 0);
  }

  void create_file(const std::filesystem::path& rel_path, const std::string& content = "dummy media content") {
    std::error_code ec;
    auto full_path = canonical_temp_dir / rel_path;
    std::filesystem::create_directories(full_path.parent_path(), ec);
    std::ofstream ofs(full_path, std::ios::binary);
    ofs << content;
  }

  ~ScanServerFixture() {
    if (server) {
      server->stop();
    }
    if (server_thread.joinable()) {
      server_thread.join();
    }
    std::error_code ec;
    std::filesystem::remove_all(temp_dir, ec);
  }
};

}  // namespace

TEST_CASE("TC-SCN-01: Valid directory scan inside workspace root", "[server][scan-path]") {
  ScanServerFixture fixture(true);
  fixture.create_file("folder1/clip1.mp4", std::string(120, 'a'));
  fixture.create_file("folder1/clip1.srt", "1\n00:00:01,000 --> 00:00:02,000\nSubtitle\n");
  fixture.create_file("folder1/nested/clip2.mov", std::string(240, 'b'));

  nlohmann::json req_body = {{"path", "folder1"}};
  auto res = fixture.cli->Post("/api/video/scan-path", req_body.dump(), "application/json");
  REQUIRE(res != nullptr);
  REQUIRE(res->status == 200);

  auto j = nlohmann::json::parse(res->body);
  REQUIRE(j.contains("accepted"));
  REQUIRE(j.contains("skipped"));
  REQUIRE(j.contains("rejected"));
  REQUIRE(j["accepted"].is_array());
  REQUIRE(j["accepted"].size() == 2);
  REQUIRE(j["skipped"] == 0);
  REQUIRE(j["rejected"].empty());

  bool found_clip1 = false;
  bool found_clip2 = false;

  for (const auto& item : j["accepted"]) {
    REQUIRE(item.contains("id"));
    REQUIRE(item["id"].is_string());
    REQUIRE_FALSE(item["id"].get<std::string>().empty());

    REQUIRE(item.contains("name"));
    REQUIRE(item.contains("videoPath"));
    REQUIRE(item.contains("relativePath"));
    REQUIRE(item.contains("sizeBytes"));
    REQUIRE(item.contains("format"));
    REQUIRE(item.contains("location"));
    REQUIRE(item.contains("outputExists"));

    REQUIRE(item["location"] == "server-path");

    std::string name = item["name"].get<std::string>();
    if (name == "clip1.mp4") {
      found_clip1 = true;
      REQUIRE(item["format"] == "mp4");
      REQUIRE(item["sizeBytes"] == 120);
      REQUIRE(item["outputExists"] == true);
      std::string vpath = item["videoPath"].get<std::string>();
      REQUIRE(vpath.find("folder1/clip1.mp4") != std::string::npos);
    } else if (name == "clip2.mov") {
      found_clip2 = true;
      REQUIRE(item["format"] == "mov");
      REQUIRE(item["sizeBytes"] == 240);
      REQUIRE(item["outputExists"] == false);
      std::string vpath = item["videoPath"].get<std::string>();
      REQUIRE(vpath.find("folder1/nested/clip2.mov") != std::string::npos);
    }
  }

  REQUIRE(found_clip1);
  REQUIRE(found_clip2);
}

TEST_CASE("TC-SCN-02: Valid single file scan inside workspace", "[server][scan-path]") {
  ScanServerFixture fixture(true);
  fixture.create_file("single_clip.mkv", std::string(300, 'c'));
  fixture.create_file("single_clip.srt", "1\n00:00:01,000 --> 00:00:03,000\nLine\n");

  SECTION("Scan single file via path string") {
    nlohmann::json req = {{"path", "single_clip.mkv"}};
    auto res = fixture.cli->Post("/api/video/scan-path", req.dump(), "application/json");
    REQUIRE(res != nullptr);
    REQUIRE(res->status == 200);

    auto j = nlohmann::json::parse(res->body);
    REQUIRE(j["accepted"].size() == 1);
    REQUIRE(j["accepted"][0]["name"] == "single_clip.mkv");
    REQUIRE(j["accepted"][0]["format"] == "mkv");
    REQUIRE(j["accepted"][0]["sizeBytes"] == 300);
    REQUIRE(j["accepted"][0]["outputExists"] == true);
    REQUIRE(j["accepted"][0]["location"] == "server-path");
    REQUIRE(j["skipped"] == 0);
    REQUIRE(j["rejected"].empty());
  }

  SECTION("Scan single file via paths array") {
    nlohmann::json req = {{"paths", {"single_clip.mkv"}}};
    auto res = fixture.cli->Post("/api/video/scan-path", req.dump(), "application/json");
    REQUIRE(res != nullptr);
    REQUIRE(res->status == 200);

    auto j = nlohmann::json::parse(res->body);
    REQUIRE(j["accepted"].size() == 1);
    REQUIRE(j["accepted"][0]["name"] == "single_clip.mkv");
    REQUIRE(j["accepted"][0]["format"] == "mkv");
  }
}

TEST_CASE("TC-SCN-03: Unconfigured workspace returning HTTP 400", "[server][scan-path]") {
  EnvVarGuard env_guard("SUBLIFT_ALLOWED_MEDIA_ROOT", std::nullopt);
  ScanServerFixture fixture(false);  // unconfigured media_dir

  nlohmann::json req = {{"path", "some_folder"}};
  auto res = fixture.cli->Post("/api/video/scan-path", req.dump(), "application/json");
  REQUIRE(res != nullptr);
  REQUIRE(res->status == 400);

  auto j = nlohmann::json::parse(res->body);
  REQUIRE(j.contains("error"));
  REQUIRE_FALSE(j["error"].get<std::string>().empty());
}

TEST_CASE("TC-SCN-04: Out-of-workspace absolute path rejection", "[server][scan-path]") {
  ScanServerFixture fixture(true);

  std::error_code ec;
  auto outside_dir = std::filesystem::temp_directory_path() /
                     ("sublift_outside_ws_" + std::to_string(::getpid()) + "_" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(outside_dir, ec);
  auto outside_file = outside_dir / "outside_video.mp4";
  {
    std::ofstream ofs(outside_file, std::ios::binary);
    ofs << "outside content";
  }

  struct OutsideCleanup {
    std::filesystem::path p;
    ~OutsideCleanup() {
      std::error_code err;
      std::filesystem::remove_all(p, err);
    }
  } cleanup{outside_dir};

  SECTION("Direct outside file absolute path") {
    nlohmann::json req = {{"path", outside_file.string()}};
    auto res = fixture.cli->Post("/api/video/scan-path", req.dump(), "application/json");
    REQUIRE(res != nullptr);
    if (res->status == 200) {
      auto j = nlohmann::json::parse(res->body);
      REQUIRE(j["accepted"].empty());
      REQUIRE(j["rejected"].size() >= 1);
    } else {
      REQUIRE((res->status == 400 || res->status == 403));
    }
  }

  SECTION("Direct outside directory absolute path") {
    nlohmann::json req = {{"paths", {outside_dir.string(), "/etc"}}};
    auto res = fixture.cli->Post("/api/video/scan-path", req.dump(), "application/json");
    REQUIRE(res != nullptr);
    if (res->status == 200) {
      auto j = nlohmann::json::parse(res->body);
      REQUIRE(j["accepted"].empty());
      REQUIRE(j["rejected"].size() >= 1);
    } else {
      REQUIRE((res->status == 400 || res->status == 403));
    }
  }
}

TEST_CASE("TC-SCN-05: Directory traversal .. escape attempt rejection", "[server][scan-path]") {
  ScanServerFixture fixture(true);
  fixture.create_file("sub/valid.mp4", "valid");

  SECTION("Parent traversal from root") {
    nlohmann::json req = {{"path", "../../etc"}};
    auto res = fixture.cli->Post("/api/video/scan-path", req.dump(), "application/json");
    REQUIRE(res != nullptr);
    if (res->status == 200) {
      auto j = nlohmann::json::parse(res->body);
      REQUIRE(j["accepted"].empty());
      REQUIRE(j["rejected"].size() >= 1);
    } else {
      REQUIRE((res->status == 400 || res->status == 403));
    }
  }

  SECTION("Relative traversal trying to escape media root") {
    nlohmann::json req = {{"paths", {"sub/../../../tmp"}}};
    auto res = fixture.cli->Post("/api/video/scan-path", req.dump(), "application/json");
    REQUIRE(res != nullptr);
    if (res->status == 200) {
      auto j = nlohmann::json::parse(res->body);
      REQUIRE(j["accepted"].empty());
      REQUIRE(j["rejected"].size() >= 1);
    } else {
      REQUIRE((res->status == 400 || res->status == 403));
    }
  }
}

TEST_CASE("TC-SCN-06: Symlink escape pointing outside workspace directory being rejected/skipped", "[server][scan-path]") {
  ScanServerFixture fixture(true);
  fixture.create_file("safe_dir/real.mp4", "real media");

  std::error_code ec;
  auto outside_dir = std::filesystem::temp_directory_path() /
                     ("sublift_symlink_ext_" + std::to_string(::getpid()) + "_" +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(outside_dir, ec);
  auto outside_clip = outside_dir / "external.mp4";
  {
    std::ofstream ofs(outside_clip, std::ios::binary);
    ofs << "external secret video";
  }

  struct OutsideCleaner {
    std::filesystem::path p;
    ~OutsideCleaner() {
      std::error_code err;
      std::filesystem::remove_all(p, err);
    }
  } cleaner{outside_dir};

  // Create symlink inside safe_dir pointing to external outside directory
  std::filesystem::create_directory_symlink(outside_dir, fixture.canonical_temp_dir / "safe_dir" / "symlink_dir", ec);
  std::filesystem::create_symlink(outside_clip, fixture.canonical_temp_dir / "safe_dir" / "symlink_file.mp4", ec);

  nlohmann::json req = {{"path", "safe_dir"}};
  auto res = fixture.cli->Post("/api/video/scan-path", req.dump(), "application/json");
  REQUIRE(res != nullptr);
  REQUIRE(res->status == 200);

  auto j = nlohmann::json::parse(res->body);
  REQUIRE(j["accepted"].size() == 1);
  REQUIRE(j["accepted"][0]["name"] == "real.mp4");
  REQUIRE(j["skipped"].get<int>() >= 1);

  // Verify external file is never accepted
  for (const auto& item : j["accepted"]) {
    REQUIRE(item["name"] != "external.mp4");
    REQUIRE(item["name"] != "symlink_file.mp4");
  }
}

TEST_CASE("TC-SCN-07: Depth recursion limit (directory deeper than 10 levels is skipped/not recursed)", "[server][scan-path]") {
  ScanServerFixture fixture(true);
  fixture.create_file("d0/shallow.mp4", "shallow video");

  // Create 12 levels deep directory: d0/l1/l2/l3/l4/l5/l6/l7/l8/l9/l10/l11/deep.mp4
  std::string deep_path = "d0";
  for (int i = 1; i <= 11; ++i) {
    deep_path += "/l" + std::to_string(i);
  }
  deep_path += "/deep.mp4";
  fixture.create_file(deep_path, "deep video");

  nlohmann::json req = {{"path", "d0"}};
  auto res = fixture.cli->Post("/api/video/scan-path", req.dump(), "application/json");
  REQUIRE(res != nullptr);
  REQUIRE(res->status == 200);

  auto j = nlohmann::json::parse(res->body);
  REQUIRE(j["accepted"].size() == 1);
  REQUIRE(j["accepted"][0]["name"] == "shallow.mp4");
  for (const auto& item : j["accepted"]) {
    REQUIRE(item["name"] != "deep.mp4");
  }
}

TEST_CASE("TC-SCN-08: Max file count limit (capping at 500 files)", "[server][scan-path]") {
  ScanServerFixture fixture(true);
  for (int i = 0; i < 520; ++i) {
    char name[64];
    std::snprintf(name, sizeof(name), "bulk/video_%04d.mp4", i);
    fixture.create_file(name, "content");
  }

  nlohmann::json req = {{"path", "bulk"}};
  auto res = fixture.cli->Post("/api/video/scan-path", req.dump(), "application/json");
  REQUIRE(res != nullptr);
  REQUIRE(res->status == 200);

  auto j = nlohmann::json::parse(res->body);
  REQUIRE(j["accepted"].size() == 500);
  REQUIRE(j["skipped"].get<int>() >= 20);
}

TEST_CASE("TC-SCN-09: Skipping hidden files and directories", "[server][scan-path]") {
  ScanServerFixture fixture(true);
  fixture.create_file("hidden_test/visible.mp4", "visible video");
  fixture.create_file("hidden_test/.DS_Store", "junk");
  fixture.create_file("hidden_test/.git/dummy.mp4", "git content");
  fixture.create_file("hidden_test/.sublift_cache/remux/cached.mp4", "cache content");
  fixture.create_file("hidden_test/.hidden_dir/video.mp4", "hidden sub video");
  fixture.create_file("hidden_test/.hidden_video.mp4", "dotfile video");

  nlohmann::json req = {{"path", "hidden_test"}};
  auto res = fixture.cli->Post("/api/video/scan-path", req.dump(), "application/json");
  REQUIRE(res != nullptr);
  REQUIRE(res->status == 200);

  auto j = nlohmann::json::parse(res->body);
  REQUIRE(j["accepted"].size() == 1);
  REQUIRE(j["accepted"][0]["name"] == "visible.mp4");
  REQUIRE(j["skipped"].get<int>() >= 4);
}

TEST_CASE("TC-SCN-10: Filtering unsupported extensions (.txt, .cpp, .ts excluded)", "[server][scan-path]") {
  ScanServerFixture fixture(true);
  fixture.create_file("ext_test/good.mp4", "mp4 video");
  fixture.create_file("ext_test/good.webm", "webm video");
  fixture.create_file("ext_test/notes.txt", "text note");
  fixture.create_file("ext_test/code.cpp", "c++ code");
  fixture.create_file("ext_test/stream.ts", "mpeg ts video - forbidden");
  fixture.create_file("ext_test/data.json", "json data");

  SECTION("Directory scan filters non-media and .ts files") {
    nlohmann::json req = {{"path", "ext_test"}};
    auto res = fixture.cli->Post("/api/video/scan-path", req.dump(), "application/json");
    REQUIRE(res != nullptr);
    REQUIRE(res->status == 200);

    auto j = nlohmann::json::parse(res->body);
    REQUIRE(j["accepted"].size() == 2);
    REQUIRE(j["skipped"].get<int>() >= 4);

    for (const auto& item : j["accepted"]) {
      std::string fmt = item["format"].get<std::string>();
      REQUIRE((fmt == "mp4" || fmt == "webm"));
      REQUIRE(fmt != "ts");
    }
  }

  SECTION("Direct unsupported file path submission is rejected") {
    nlohmann::json req = {{"path", "ext_test/notes.txt"}};
    auto res = fixture.cli->Post("/api/video/scan-path", req.dump(), "application/json");
    REQUIRE(res != nullptr);
    if (res->status == 200) {
      auto j = nlohmann::json::parse(res->body);
      REQUIRE(j["accepted"].empty());
      REQUIRE(j["rejected"].size() == 1);
    } else {
      REQUIRE((res->status == 400 || res->status == 403));
    }
  }

  SECTION("Direct .ts file path submission is rejected due to security policy") {
    nlohmann::json req = {{"path", "ext_test/stream.ts"}};
    auto res = fixture.cli->Post("/api/video/scan-path", req.dump(), "application/json");
    REQUIRE(res != nullptr);
    if (res->status == 200) {
      auto j = nlohmann::json::parse(res->body);
      REQUIRE(j["accepted"].empty());
      REQUIRE(j["rejected"].size() == 1);
    } else {
      REQUIRE((res->status == 400 || res->status == 403));
    }
  }
}

TEST_CASE("TC-SCN-11: Duplicate path deduplication in request", "[server][scan-path]") {
  ScanServerFixture fixture(true);
  fixture.create_file("dup_test/clip.mp4", "media content");

  nlohmann::json req = {{"paths", {"dup_test/clip.mp4", "dup_test/clip.mp4", "./dup_test/clip.mp4"}}};
  auto res = fixture.cli->Post("/api/video/scan-path", req.dump(), "application/json");
  REQUIRE(res != nullptr);
  REQUIRE(res->status == 200);

  auto j = nlohmann::json::parse(res->body);
  REQUIRE(j["accepted"].size() == 1);
  REQUIRE(j["accepted"][0]["name"] == "clip.mp4");
  REQUIRE((j["rejected"].size() >= 1 || j["skipped"].get<int>() >= 2));
}

TEST_CASE("TC-SCN-12: Empty directory scan returning structured empty accepted list", "[server][scan-path]") {
  ScanServerFixture fixture(true);
  std::error_code ec;
  std::filesystem::create_directories(fixture.canonical_temp_dir / "empty_dir", ec);

  nlohmann::json req = {{"path", "empty_dir"}};
  auto res = fixture.cli->Post("/api/video/scan-path", req.dump(), "application/json");
  REQUIRE(res != nullptr);
  REQUIRE(res->status == 200);

  auto j = nlohmann::json::parse(res->body);
  REQUIRE(j["accepted"].is_array());
  REQUIRE(j["accepted"].empty());
  REQUIRE(j["rejected"].size() >= 1);
}

TEST_CASE("TC-SCN-13: Non-existent path returning structured rejection or 404", "[server][scan-path]") {
  ScanServerFixture fixture(true);

  nlohmann::json req = {{"path", "completely_nonexistent_directory_xyz"}};
  auto res = fixture.cli->Post("/api/video/scan-path", req.dump(), "application/json");
  REQUIRE(res != nullptr);
  if (res->status == 200) {
    auto j = nlohmann::json::parse(res->body);
    REQUIRE(j["accepted"].empty());
    REQUIRE(j["rejected"].size() >= 1);
  } else {
    REQUIRE((res->status == 404 || res->status == 400));
  }
}

TEST_CASE("TC-SCN-14: Malformed JSON request body returning HTTP 400", "[server][scan-path]") {
  ScanServerFixture fixture(true);

  SECTION("Broken JSON string") {
    auto res = fixture.cli->Post("/api/video/scan-path", "{broken json", "application/json");
    REQUIRE(res != nullptr);
    REQUIRE(res->status == 400);
  }

  SECTION("Missing required path/paths field") {
    auto res = fixture.cli->Post("/api/video/scan-path", "{}", "application/json");
    REQUIRE(res != nullptr);
    REQUIRE(res->status == 400);
  }

  SECTION("Non-string path field") {
    auto res = fixture.cli->Post("/api/video/scan-path", R"({"path": 12345})", "application/json");
    REQUIRE(res != nullptr);
    REQUIRE(res->status == 400);
  }

  SECTION("Non-array paths field") {
    auto res = fixture.cli->Post("/api/video/scan-path", R"({"paths": "not an array"})", "application/json");
    REQUIRE(res != nullptr);
    REQUIRE(res->status == 400);
  }
}

// ===========================================================================
// Milestone 1 (Feature 12511): 统一媒体沙箱与 Native Server 信任边界全面测试
// ===========================================================================

namespace {

struct SandboxMatrixFixture {
  std::filesystem::path base_temp;
  std::filesystem::path ws_a;
  std::filesystem::path ws_b;
  std::filesystem::path outside_dir;
  std::filesystem::path config_file;

  std::filesystem::path file_a;
  std::filesystem::path file_b;
  std::filesystem::path file_outside;
  std::filesystem::path ts_file_a;
  std::filesystem::path cache_file_a;
  std::filesystem::path symlink_outside_a;
  std::filesystem::path symlink_inside_a;
  std::filesystem::path symlink_to_cache_a;

  std::unique_ptr<sublift::server::HttpServer> server;
  std::unique_ptr<std::thread> server_thread;
  std::unique_ptr<httplib::Client> cli;
  int port{0};

  explicit SandboxMatrixFixture(bool configure_ws_a_initially = true) {
    auto now_ns = std::chrono::steady_clock::now().time_since_epoch().count();
    base_temp = std::filesystem::temp_directory_path() /
                ("sublift_sb_matrix_" + std::to_string(::getpid()) + "_" + std::to_string(now_ns));
    ws_a = base_temp / "workspace_a";
    ws_b = base_temp / "workspace_b";
    outside_dir = base_temp / "outside_dir";
    config_file = base_temp / "ws_config.json";

    std::filesystem::create_directories(ws_a);
    std::filesystem::create_directories(ws_b);
    std::filesystem::create_directories(outside_dir);
    std::filesystem::create_directories(ws_a / ".sublift_cache" / "remux");

    file_a = ws_a / "video_a.mp4";
    file_b = ws_b / "video_b.mp4";
    file_outside = outside_dir / "video_outside.mp4";
    ts_file_a = ws_a / "secret.ts";
    cache_file_a = ws_a / ".sublift_cache" / "remux" / "internal_cached.mp4";

    auto write_file = [](const std::filesystem::path& p, const std::string& content) {
      std::ofstream f(p, std::ios::binary);
      f << content;
    };

    write_file(file_a, "dummy mp4 content A for testing video stream and jobs 1234567890");
    write_file(file_b, "dummy mp4 content B for testing video stream and jobs 1234567890");
    write_file(file_outside, "dummy mp4 content OUTSIDE for testing video stream 1234567890");
    write_file(ts_file_a, "const secret = 42; // TypeScript source");
    write_file(cache_file_a, "dummy remuxed mp4 in cache directory");

    std::error_code sym_ec;
    symlink_outside_a = ws_a / "symlink_outside.mp4";
    std::filesystem::create_symlink(file_outside, symlink_outside_a, sym_ec);

    symlink_inside_a = ws_a / "symlink_inside.mp4";
    std::filesystem::create_symlink(file_a, symlink_inside_a, sym_ec);

    symlink_to_cache_a = ws_a / "symlink_cache.mp4";
    std::filesystem::create_symlink(cache_file_a, symlink_to_cache_a, sym_ec);

    sublift::server::ServerConfig cfg;
    cfg.port = 0;
    cfg.cors_origin = "*";
    cfg.config_file = config_file.string();
    if (configure_ws_a_initially) {
      cfg.media_dir = ws_a.string();
    } else {
      cfg.media_dir = "";
    }

    server = std::make_unique<sublift::server::HttpServer>(std::move(cfg));
    port = server->bind_to_any_port("127.0.0.1");
    server_thread = std::make_unique<std::thread>([this]() {
      server->listen_after_bind();
    });
    server->wait_until_ready();

    cli = std::make_unique<httplib::Client>("127.0.0.1", port);
    cli->set_connection_timeout(std::chrono::seconds(3));
    cli->set_read_timeout(std::chrono::seconds(5));
  }

  ~SandboxMatrixFixture() {
    if (server) {
      server->stop();
    }
    if (server_thread && server_thread->joinable()) {
      server_thread->join();
    }
    std::error_code ec;
    std::filesystem::remove_all(base_temp, ec);
  }

  void switch_workspace(const std::filesystem::path& new_ws) {
    nlohmann::json req = {{"media_dir", new_ws.string()}};
    auto res = cli->Post("/api/config/workspace", req.dump(), "application/json");
    REQUIRE(res != nullptr);
    REQUIRE(res->status == 200);
  }

  void clear_workspace() {
    auto res = cli->Post("/api/config/workspace/clear", "", "application/json");
    REQUIRE(res != nullptr);
    REQUIRE(res->status == 200);
  }
};

}  // namespace

TEST_CASE("PathSandbox direct security unit assertions", "[server][path_sandbox]") {
  auto now_ns = std::chrono::steady_clock::now().time_since_epoch().count();
  auto root_a = std::filesystem::temp_directory_path() / ("sublift_sandbox_unit_a_" + std::to_string(now_ns));
  auto root_b = std::filesystem::temp_directory_path() / ("sublift_sandbox_unit_b_" + std::to_string(now_ns));
  std::filesystem::create_directories(root_a);
  std::filesystem::create_directories(root_b);
  std::filesystem::create_directories(root_a / ".sublift_cache");

  auto vid_a = root_a / "test.mp4";
  auto vid_b = root_b / "test.mp4";
  auto ts_a = root_a / "test.ts";
  auto cache_vid = root_a / ".sublift_cache" / "test.mp4";

  {
    std::ofstream(vid_a) << "vid a";
    std::ofstream(vid_b) << "vid b";
    std::ofstream(ts_a) << "ts a";
    std::ofstream(cache_vid) << "cache vid";
  }

  std::error_code ec;
  auto sym_out = root_a / "sym_out.mp4";
  std::filesystem::create_symlink(vid_b, sym_out, ec);
  auto sym_in = root_a / "sym_in.mp4";
  std::filesystem::create_symlink(vid_a, sym_in, ec);
  auto sym_cache = root_a / "sym_cache.mp4";
  std::filesystem::create_symlink(cache_vid, sym_cache, ec);

  // 1. Fail-closed on unconfigured root
  REQUIRE_THROWS_AS(sublift::server::resolve_and_validate_media_path(vid_a.string(), std::nullopt),
                    sublift::server::PathSecurityException);

  // 2. Valid file inside root passes
  auto validated = sublift::server::resolve_and_validate_media_path(vid_a.string(), root_a);
  REQUIRE(validated == std::filesystem::canonical(vid_a));

  // 3. Relative path inside root resolves correctly
  auto rel_val = sublift::server::resolve_and_validate_media_path("test.mp4", root_a);
  REQUIRE(rel_val == std::filesystem::canonical(vid_a));

  // 4. File outside root fails
  REQUIRE_THROWS_AS(sublift::server::resolve_and_validate_media_path(vid_b.string(), root_a),
                    sublift::server::PathSecurityException);

  // 5. Traversal escape fails
  REQUIRE_THROWS_AS(sublift::server::resolve_and_validate_media_path("../test.mp4", root_a),
                    sublift::server::PathSecurityException);

  // 6. External symlink fails
  REQUIRE_THROWS_AS(sublift::server::resolve_and_validate_media_path(sym_out.string(), root_a),
                    sublift::server::PathSecurityException);

  // 7. Internal symlink passes
  auto sym_in_val = sublift::server::resolve_and_validate_media_path(sym_in.string(), root_a);
  REQUIRE(sym_in_val == std::filesystem::canonical(vid_a));

  // 8. .sublift_cache direct and symlink access fail
  REQUIRE_THROWS_AS(sublift::server::resolve_and_validate_media_path(cache_vid.string(), root_a),
                    sublift::server::PathSecurityException);
  REQUIRE_THROWS_AS(sublift::server::resolve_and_validate_media_path(sym_cache.string(), root_a),
                    sublift::server::PathSecurityException);

  // 9. Non-media extension fails
  REQUIRE_THROWS_AS(sublift::server::resolve_and_validate_media_path(ts_a.string(), root_a),
                    sublift::server::PathSecurityException);

  std::filesystem::remove_all(root_a, ec);
  std::filesystem::remove_all(root_b, ec);
}

TEST_CASE("Unified Media Sandbox Security Matrix Across All 7 Routes", "[server][sandbox_matrix]") {
  SECTION("Route 1: GET /api/video/stream") {
    // A. Unconfigured workspace fails closed
    {
      SandboxMatrixFixture fix(false);
      auto res = fix.cli->Get("/api/video/stream?path=" + fix.file_a.string());
      REQUIRE(res != nullptr);
      REQUIRE(res->status == 400);
    }
    // B. Matrix on configured workspace
    {
      SandboxMatrixFixture fix(true);
      // Valid file in ws_a -> 200 OK
      auto r_ok = fix.cli->Get("/api/video/stream?path=" + fix.file_a.string());
      REQUIRE(r_ok != nullptr);
      REQUIRE(r_ok->status == 200);

      // Relative path -> 200 OK
      auto r_rel = fix.cli->Get("/api/video/stream?path=video_a.mp4");
      REQUIRE(r_rel != nullptr);
      REQUIRE(r_rel->status == 200);

      // Internal symlink -> 200 OK
      auto r_sym_in = fix.cli->Get("/api/video/stream?path=" + fix.symlink_inside_a.string());
      REQUIRE(r_sym_in != nullptr);
      REQUIRE(r_sym_in->status == 200);

      // Outside path -> 400
      auto r_out = fix.cli->Get("/api/video/stream?path=" + fix.file_outside.string());
      REQUIRE(r_out != nullptr);
      REQUIRE(r_out->status == 400);

      // Traversal -> 400
      auto r_trav = fix.cli->Get("/api/video/stream?path=../../outside_dir/video_outside.mp4");
      REQUIRE(r_trav != nullptr);
      REQUIRE(r_trav->status == 400);

      // External symlink -> 400
      auto r_sym_out = fix.cli->Get("/api/video/stream?path=" + fix.symlink_outside_a.string());
      REQUIRE(r_sym_out != nullptr);
      REQUIRE(r_sym_out->status == 400);

      // Non-media extension -> 400
      auto r_ts = fix.cli->Get("/api/video/stream?path=" + fix.ts_file_a.string());
      REQUIRE(r_ts != nullptr);
      REQUIRE(r_ts->status == 400);

      // .sublift_cache direct -> 400
      auto r_cache = fix.cli->Get("/api/video/stream?path=" + fix.cache_file_a.string());
      REQUIRE(r_cache != nullptr);
      REQUIRE(r_cache->status == 400);

      // .sublift_cache symlink -> 400
      auto r_cache_sym = fix.cli->Get("/api/video/stream?path=" + fix.symlink_to_cache_a.string());
      REQUIRE(r_cache_sym != nullptr);
      REQUIRE(r_cache_sym->status == 400);

      // Dynamic switch to ws_b
      fix.switch_workspace(fix.ws_b);
      auto r_new = fix.cli->Get("/api/video/stream?path=" + fix.file_b.string());
      REQUIRE(r_new != nullptr);
      REQUIRE(r_new->status == 200);

      // Old ws_a file immediately rejected
      auto r_old = fix.cli->Get("/api/video/stream?path=" + fix.file_a.string());
      REQUIRE(r_old != nullptr);
      REQUIRE(r_old->status == 400);
    }
  }

  SECTION("Route 2: GET /api/video/frame") {
    // Unconfigured
    {
      SandboxMatrixFixture fix(false);
      auto res = fix.cli->Get("/api/video/frame?path=" + fix.file_a.string());
      REQUIRE(res != nullptr);
      REQUIRE(res->status == 400);
    }
    // Configured matrix
    {
      SandboxMatrixFixture fix(true);
      auto r_out = fix.cli->Get("/api/video/frame?path=" + fix.file_outside.string());
      REQUIRE(r_out != nullptr);
      REQUIRE(r_out->status == 400);

      auto r_trav = fix.cli->Get("/api/video/frame?path=../outside_dir/video_outside.mp4");
      REQUIRE(r_trav != nullptr);
      REQUIRE(r_trav->status == 400);

      auto r_sym = fix.cli->Get("/api/video/frame?path=" + fix.symlink_outside_a.string());
      REQUIRE(r_sym != nullptr);
      REQUIRE(r_sym->status == 400);

      auto r_ts = fix.cli->Get("/api/video/frame?path=" + fix.ts_file_a.string());
      REQUIRE(r_ts != nullptr);
      REQUIRE(r_ts->status == 400);

      auto r_cache = fix.cli->Get("/api/video/frame?path=" + fix.cache_file_a.string());
      REQUIRE(r_cache != nullptr);
      REQUIRE(r_cache->status == 400);

      fix.switch_workspace(fix.ws_b);
      auto r_old = fix.cli->Get("/api/video/frame?path=" + fix.file_a.string());
      REQUIRE(r_old != nullptr);
      REQUIRE(r_old->status == 400);
    }
  }

  SECTION("Route 3: POST /api/video/detect-region") {
    // Unconfigured
    {
      SandboxMatrixFixture fix(false);
      nlohmann::json body = {{"video_path", fix.file_a.string()}};
      auto res = fix.cli->Post("/api/video/detect-region", body.dump(), "application/json");
      REQUIRE(res != nullptr);
      REQUIRE(res->status == 400);
    }
    // Configured matrix
    {
      SandboxMatrixFixture fix(true);
      auto post_dr = [&](const std::string& p) {
        nlohmann::json b = {{"video_path", p}};
        return fix.cli->Post("/api/video/detect-region", b.dump(), "application/json");
      };

      auto r_out = post_dr(fix.file_outside.string());
      REQUIRE(r_out != nullptr);
      REQUIRE(r_out->status == 400);

      auto r_trav = post_dr("../outside_dir/video_outside.mp4");
      REQUIRE(r_trav != nullptr);
      REQUIRE(r_trav->status == 400);

      auto r_sym = post_dr(fix.symlink_outside_a.string());
      REQUIRE(r_sym != nullptr);
      REQUIRE(r_sym->status == 400);

      auto r_ts = post_dr(fix.ts_file_a.string());
      REQUIRE(r_ts != nullptr);
      REQUIRE(r_ts->status == 400);

      auto r_cache = post_dr(fix.cache_file_a.string());
      REQUIRE(r_cache != nullptr);
      REQUIRE(r_cache->status == 400);

      fix.switch_workspace(fix.ws_b);
      auto r_old = post_dr(fix.file_a.string());
      REQUIRE(r_old != nullptr);
      REQUIRE(r_old->status == 400);
    }
  }

  SECTION("Route 4: POST /api/video/resolve") {
    auto to_hex = [](const unsigned char* p, size_t n) {
      static const char* kHex = "0123456789abcdef";
      std::string out;
      out.reserve(n * 2);
      for (size_t i = 0; i < n; ++i) {
        out.push_back(kHex[p[i] >> 4]);
        out.push_back(kHex[p[i] & 0xf]);
      }
      return out;
    };

    std::string content_a = "dummy mp4 content A for testing video stream and jobs 1234567890";
    std::string head_a = to_hex(reinterpret_cast<const unsigned char*>(content_a.data()), content_a.size());
    std::string content_b = "dummy mp4 content B for testing video stream and jobs 1234567890";
    std::string head_b = to_hex(reinterpret_cast<const unsigned char*>(content_b.data()), content_b.size());

    // Unconfigured
    {
      SandboxMatrixFixture fix(false);
      nlohmann::json b = {{"name", "video_a.mp4"}, {"size", content_a.size()}, {"head_hex", head_a}};
      auto res = fix.cli->Post("/api/video/resolve", b.dump(), "application/json");
      REQUIRE(res != nullptr);
      REQUIRE(res->status == 404);
    }
    // Configured
    {
      SandboxMatrixFixture fix(true);
      // Valid file in ws_a resolves
      nlohmann::json b_a = {{"name", "video_a.mp4"}, {"size", content_a.size()}, {"head_hex", head_a}};
      auto r_a = fix.cli->Post("/api/video/resolve", b_a.dump(), "application/json");
      REQUIRE(r_a != nullptr);
      REQUIRE(r_a->status == 200);
      auto j_a = nlohmann::json::parse(r_a->body);
      std::error_code ec;
      REQUIRE(j_a["path"] == std::filesystem::canonical(fix.file_a, ec).string());

      // File outside does not resolve
      nlohmann::json b_out = {{"name", "video_outside.mp4"}, {"size", content_a.size()}, {"head_hex", head_a}};
      auto r_out = fix.cli->Post("/api/video/resolve", b_out.dump(), "application/json");
      REQUIRE(r_out != nullptr);
      REQUIRE(r_out->status == 404);

      // Non-media file .ts does not resolve
      nlohmann::json b_ts = {{"name", "secret.ts"}, {"size", 10}, {"head_hex", "0011223344"}};
      auto r_ts = fix.cli->Post("/api/video/resolve", b_ts.dump(), "application/json");
      REQUIRE(r_ts != nullptr);
      REQUIRE(r_ts->status == 404);

      // Switch to ws_b: video_a.mp4 no longer resolves, video_b.mp4 resolves
      fix.switch_workspace(fix.ws_b);
      auto r_a_switched = fix.cli->Post("/api/video/resolve", b_a.dump(), "application/json");
      REQUIRE(r_a_switched != nullptr);
      REQUIRE(r_a_switched->status == 404);

      nlohmann::json b_b = {{"name", "video_b.mp4"}, {"size", content_b.size()}, {"head_hex", head_b}};
      auto r_b = fix.cli->Post("/api/video/resolve", b_b.dump(), "application/json");
      REQUIRE(r_b != nullptr);
      REQUIRE(r_b->status == 200);
    }
  }

  SECTION("Route 5: POST /api/video/scan-path") {
    // Unconfigured
    {
      SandboxMatrixFixture fix(false);
      nlohmann::json b = {{"path", fix.ws_a.string()}};
      auto res = fix.cli->Post("/api/video/scan-path", b.dump(), "application/json");
      REQUIRE(res != nullptr);
      REQUIRE(res->status == 400);
    }
    // Configured
    {
      SandboxMatrixFixture fix(true);
      // Valid scan of ws_a
      nlohmann::json b_a = {{"path", fix.ws_a.string()}};
      auto r_a = fix.cli->Post("/api/video/scan-path", b_a.dump(), "application/json");
      REQUIRE(r_a != nullptr);
      REQUIRE(r_a->status == 200);
      auto j_a = nlohmann::json::parse(r_a->body);
      REQUIRE(j_a["accepted"].size() == 1);
      REQUIRE(j_a["accepted"][0]["name"] == "video_a.mp4");

      // Scan outside path
      nlohmann::json b_out = {{"path", fix.outside_dir.string()}};
      auto r_out = fix.cli->Post("/api/video/scan-path", b_out.dump(), "application/json");
      REQUIRE(r_out != nullptr);
      REQUIRE(r_out->status == 200);
      auto j_out = nlohmann::json::parse(r_out->body);
      REQUIRE(j_out["accepted"].empty());
      REQUIRE(j_out["rejected"].size() >= 1);

      // Switch to ws_b: scanning ws_a is now rejected
      fix.switch_workspace(fix.ws_b);
      auto r_a_after = fix.cli->Post("/api/video/scan-path", b_a.dump(), "application/json");
      REQUIRE(r_a_after != nullptr);
      REQUIRE(r_a_after->status == 200);
      auto j_a_after = nlohmann::json::parse(r_a_after->body);
      REQUIRE(j_a_after["accepted"].empty());
      REQUIRE(j_a_after["rejected"].size() >= 1);
    }
  }

  SECTION("Route 6: POST /api/jobs") {
    // Unconfigured
    {
      SandboxMatrixFixture fix(false);
      nlohmann::json b = {{"video_path", fix.file_a.string()}, {"engine", "mock"}};
      auto res = fix.cli->Post("/api/jobs", b.dump(), "application/json");
      REQUIRE(res != nullptr);
      REQUIRE(res->status == 400);
    }
    // Configured matrix
    {
      SandboxMatrixFixture fix(true);
      auto post_job = [&](const std::string& p) {
        nlohmann::json b = {{"video_path", p}, {"engine", "mock"}};
        return fix.cli->Post("/api/jobs", b.dump(), "application/json");
      };

      // Valid job creation in ws_a -> 201 Created
      auto r_ok = post_job(fix.file_a.string());
      REQUIRE(r_ok != nullptr);
      REQUIRE(r_ok->status == 201);

      // Outside path -> 400
      auto r_out = post_job(fix.file_outside.string());
      REQUIRE(r_out != nullptr);
      REQUIRE(r_out->status == 400);

      // Traversal -> 400
      auto r_trav = post_job("../outside_dir/video_outside.mp4");
      REQUIRE(r_trav != nullptr);
      REQUIRE(r_trav->status == 400);

      // External symlink -> 400
      auto r_sym = post_job(fix.symlink_outside_a.string());
      REQUIRE(r_sym != nullptr);
      REQUIRE(r_sym->status == 400);

      // Non-media extension -> 400
      auto r_ts = post_job(fix.ts_file_a.string());
      REQUIRE(r_ts != nullptr);
      REQUIRE(r_ts->status == 400);

      // .sublift_cache -> 400
      auto r_cache = post_job(fix.cache_file_a.string());
      REQUIRE(r_cache != nullptr);
      REQUIRE(r_cache->status == 400);

      // Switch to ws_b
      fix.switch_workspace(fix.ws_b);
      auto r_old = post_job(fix.file_a.string());
      REQUIRE(r_old != nullptr);
      REQUIRE(r_old->status == 400);

      auto r_new = post_job(fix.file_b.string());
      REQUIRE(r_new != nullptr);
      REQUIRE(r_new->status == 201);
    }
  }

  SECTION("Route 7: GET /api/jobs/:id/export") {
    SandboxMatrixFixture fix(true);
    // 1. Create and finish a job in ws_a
    nlohmann::json b = {{"video_path", fix.file_a.string()}, {"engine", "mock"}};
    auto r_job = fix.cli->Post("/api/jobs", b.dump(), "application/json");
    REQUIRE(r_job != nullptr);
    REQUIRE(r_job->status == 201);
    auto j_job = nlohmann::json::parse(r_job->body);
    std::string job_id = j_job["job_id"].get<std::string>();

    // Mark job completed with test entries to test export endpoint
    auto job_ptr = fix.server->job_manager()->get_job(job_id);
    REQUIRE(job_ptr != nullptr);
    {
      std::lock_guard<std::mutex> lk(job_ptr->state_mutex);
      job_ptr->status = sublift::server::JobStatus::Completed;
      job_ptr->entries.push_back({.start_ms = 1000, .end_ms = 2000, .text = "Test subtitle"});
    }

    // Export succeeds while in ws_a
    auto r_exp = fix.cli->Get("/api/jobs/" + job_id + "/export");
    REQUIRE(r_exp != nullptr);
    REQUIRE(r_exp->status == 200);

    // Switch workspace to ws_b -> export for old ws_a job is immediately rejected (400)
    fix.switch_workspace(fix.ws_b);
    auto r_exp_switched = fix.cli->Get("/api/jobs/" + job_id + "/export");
    REQUIRE(r_exp_switched != nullptr);
    REQUIRE(r_exp_switched->status == 400);

    // Switch back to ws_a -> export succeeds again
    fix.switch_workspace(fix.ws_a);
    auto r_exp_restored = fix.cli->Get("/api/jobs/" + job_id + "/export");
    REQUIRE(r_exp_restored != nullptr);
    REQUIRE(r_exp_restored->status == 200);
  }
}

TEST_CASE("JobManager State Snapshot Persistence and Recovery", "[server][job_persistence]") {
  TempTestDir temp_ws("sublift_jm_test");
  std::filesystem::path state_path = temp_ws.path / ".sublift_cache" / "jobs_state.v1.json";

  SECTION("State snapshot is written and reloaded across JobManager instances") {
    // 1. Manually write a state file representing completed job
    {
      std::error_code ec;
      std::filesystem::create_directories(state_path.parent_path(), ec);
      nlohmann::json j = {
          {"version", 1},
          {"updated_at_ms", 1724500000000LL},
          {"jobs", nlohmann::json::array({
              {
                  {"job_id", "test-job-completed-1"},
                  {"config", {
                      {"video_path", (temp_ws.path / "sample.mp4").string()},
                      {"engine", "mock"},
                      {"fps", 5.0},
                      {"confidence_threshold", 0.8},
                      {"region_box", {{"x", 0.1}, {"y", 0.6}, {"width", 0.8}, {"height", 0.3}}}
                  }},
                  {"status", "completed"},
                  {"created_at_ms", 1724500000000LL},
                  {"started_at_ms", 1724500001000LL},
                  {"ended_at_ms", 1724500005000LL},
                  {"error_message", ""},
                  {"entries", nlohmann::json::array({
                      {{"index", 1}, {"start_ms", 1000}, {"end_ms", 2500}, {"text", "First entry"}, {"confidence", 0.95}},
                      {{"index", 2}, {"start_ms", 2600}, {"end_ms", 4000}, {"text", "Second entry"}, {"confidence", 0.98}}
                  })}
              }
          })}
      };
      std::ofstream ofs(state_path);
      ofs << j.dump(2);
    }

    // Now reload into a fresh JobManager instance pointing to the state file
    {
      sublift::server::JobManager jm(1, 50, state_path);
      auto loaded_job = jm.get_job("test-job-completed-1");
      REQUIRE(loaded_job != nullptr);
      REQUIRE(loaded_job->job_id == "test-job-completed-1");
      REQUIRE(loaded_job->status == sublift::server::JobStatus::Completed);
      REQUIRE(loaded_job->config.engine == "mock");
      REQUIRE(loaded_job->config.fps == 5.0);
      REQUIRE(loaded_job->config.confidence_threshold == 0.8);
      REQUIRE(loaded_job->config.region_box.x == 0.1);
      REQUIRE(loaded_job->config.region_box.y == 0.6);
      REQUIRE(loaded_job->config.region_box.width == 0.8);
      REQUIRE(loaded_job->config.region_box.height == 0.3);
      REQUIRE(loaded_job->entries.size() == 2);
      REQUIRE(loaded_job->entries[0].text == "First entry");
      REQUIRE(loaded_job->entries[1].text == "Second entry");

      // Verify saving back produces a valid file
      jm.save_state();
      REQUIRE(std::filesystem::exists(state_path));
    }
  }

  SECTION("Incomplete jobs (Queued / Running) transition to Interrupted and are NOT auto-restarted") {
    // Write state file with queued and running jobs
    {
      std::error_code ec;
      std::filesystem::create_directories(state_path.parent_path(), ec);
      nlohmann::json j = {
          {"version", 1},
          {"updated_at_ms", 1724500000000LL},
          {"jobs", nlohmann::json::array({
              {
                  {"job_id", "job-queued-1"},
                  {"config", {{"video_path", "/nonexistent/v1.mp4"}, {"engine", "mock"}}},
                  {"status", "queued"},
                  {"created_at_ms", 100LL},
                  {"started_at_ms", 0LL},
                  {"ended_at_ms", 0LL},
                  {"error_message", ""},
                  {"entries", nlohmann::json::array()}
              },
              {
                  {"job_id", "job-running-1"},
                  {"config", {{"video_path", "/nonexistent/v2.mp4"}, {"engine", "mock"}}},
                  {"status", "running"},
                  {"created_at_ms", 100LL},
                  {"started_at_ms", 200LL},
                  {"ended_at_ms", 0LL},
                  {"error_message", ""},
                  {"entries", nlohmann::json::array()}
              }
          })}
      };
      std::ofstream ofs(state_path);
      ofs << j.dump(2);
    }

    // Reload into a fresh JobManager
    {
      sublift::server::JobManager jm(1, 50, state_path);
      auto l1 = jm.get_job("job-queued-1");
      REQUIRE(l1 != nullptr);
      REQUIRE(l1->status == sublift::server::JobStatus::Interrupted);

      auto l2 = jm.get_job("job-running-1");
      REQUIRE(l2 != nullptr);
      REQUIRE(l2->status == sublift::server::JobStatus::Interrupted);

      // Verify neither job was executed / no background task running
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      REQUIRE(l1->status == sublift::server::JobStatus::Interrupted);
      REQUIRE(l2->status == sublift::server::JobStatus::Interrupted);
    }
  }

  SECTION("Bounded LRU history maintains max_history_jobs limit") {
    // Write state file with 15 completed jobs
    nlohmann::json jobs_arr = nlohmann::json::array();
    for (int i = 0; i < 15; ++i) {
      jobs_arr.push_back({
          {"job_id", "job-lru-" + std::to_string(i)},
          {"config", {{"video_path", "/test/video.mp4"}, {"engine", "mock"}}},
          {"status", "completed"},
          {"created_at_ms", 100LL + i},
          {"started_at_ms", 200LL + i},
          {"ended_at_ms", 300LL + i},
          {"error_message", ""},
          {"entries", nlohmann::json::array()}
      });
    }

    {
      std::error_code ec;
      std::filesystem::create_directories(state_path.parent_path(), ec);
      nlohmann::json j = {
          {"version", 1},
          {"updated_at_ms", 1724500000000LL},
          {"jobs", jobs_arr}
      };
      std::ofstream ofs(state_path);
      ofs << j.dump(2);
    }

    sublift::server::JobManager jm(1, 10, state_path);
    auto all_jobs = jm.get_all_jobs();
    REQUIRE(all_jobs.size() <= 10);

    // Oldest jobs (10-14) should have been evicted
    REQUIRE(jm.get_job("job-lru-14") == nullptr);
    REQUIRE(jm.get_job("job-lru-13") == nullptr);
    // Newest jobs (0-9) should still exist
    REQUIRE(jm.get_job("job-lru-0") != nullptr);
    REQUIRE(jm.get_job("job-lru-9") != nullptr);
  }

  SECTION("Corrupted or malformed state file is handled gracefully") {
    // Write corrupted JSON
    {
      std::ofstream ofs(state_path);
      ofs << "{ \"version\": 1, \"jobs\": [ { invalid json }";
    }

    sublift::server::JobManager jm(1, 50, state_path);
    auto all_jobs = jm.get_all_jobs();
    REQUIRE(all_jobs.empty());

    // Write unsupported version
    {
      std::ofstream ofs(state_path);
      ofs << "{ \"version\": 999, \"jobs\": [] }";
    }
    sublift::server::JobManager jm_bad_ver(1, 50, state_path);
    REQUIRE(jm_bad_ver.get_all_jobs().empty());
  }
}

TEST_CASE("Server SSE Last-Event-ID and Cursor Resumption", "[server][sse_resume]") {
  TempTestDir temp_ws("sublift_sse_test");
  sublift::server::ServerConfig cfg;
  cfg.media_dir = temp_ws.path.string();
  sublift::server::HttpServer server(std::move(cfg));
  int port = server.bind_to_any_port("127.0.0.1");
  REQUIRE(port > 0);

  std::thread server_thread([&]() {
    server.listen_after_bind();
  });
  server.wait_until_ready();

  httplib::Client cli("127.0.0.1", port);
  cli.set_connection_timeout(std::chrono::seconds(2));
  cli.set_read_timeout(std::chrono::seconds(5));

  // Create a job in JobManager
  sublift::server::JobConfig jcfg{
      .video_path = (temp_ws.path / "test.mp4").string(),
      .engine = "mock"
  };
  auto job = server.job_manager()->create_job(jcfg);
  REQUIRE(job != nullptr);

  // Publish 5 distinct progress events
  job->event_stream->publish("progress", {{"stage", "step1"}, {"pct", 0.2}});
  job->event_stream->publish("progress", {{"stage", "step2"}, {"pct", 0.4}});
  job->event_stream->publish("progress", {{"stage", "step3"}, {"pct", 0.6}});
  job->event_stream->publish("progress", {{"stage", "step4"}, {"pct", 0.8}});
  job->event_stream->publish("done", {{"ok", true}, {"total_entries", 0}, {"elapsed_ms", 100}});

  {
    std::lock_guard<std::mutex> lk(job->state_mutex);
    job->status = sublift::server::JobStatus::Completed;
  }

  SECTION("SSE with Last-Event-ID header resumes from given sequence ID") {
    httplib::Headers headers = {{"Last-Event-ID", "3"}};
    std::string response_body;
    auto res = cli.Get(("/api/jobs/" + job->job_id + "/events").c_str(), headers,
                       [&](const char* data, size_t data_length) {
                         response_body.append(data, data_length);
                         return true;
                       });

    REQUIRE(res != nullptr);
    REQUIRE(res->status == 200);
    // Should contain events 4 and 5 (step4 and done), but NOT step1, step2, step3
    REQUIRE(response_body.find("id: 4") != std::string::npos);
    REQUIRE(response_body.find("id: 5") != std::string::npos);
    REQUIRE(response_body.find("step4") != std::string::npos);
    REQUIRE(response_body.find("id: 1") == std::string::npos);
    REQUIRE(response_body.find("step1") == std::string::npos);
    REQUIRE(response_body.find("id: 2") == std::string::npos);
    REQUIRE(response_body.find("id: 3") == std::string::npos);
  }

  SECTION("SSE with cursor query parameter resumes from given sequence ID") {
    std::string response_body;
    auto res = cli.Get(("/api/jobs/" + job->job_id + "/events?cursor=3").c_str(),
                       [&](const char* data, size_t data_length) {
                         response_body.append(data, data_length);
                         return true;
                       });

    REQUIRE(res != nullptr);
    REQUIRE(res->status == 200);
    REQUIRE(response_body.find("id: 4") != std::string::npos);
    REQUIRE(response_body.find("id: 5") != std::string::npos);
    REQUIRE(response_body.find("step4") != std::string::npos);
    REQUIRE(response_body.find("id: 1") == std::string::npos);
    REQUIRE(response_body.find("id: 2") == std::string::npos);
    REQUIRE(response_body.find("id: 3") == std::string::npos);
  }

  server.stop();
  if (server_thread.joinable()) {
    server_thread.join();
  }
}

TEST_CASE("Server SSE 10 Events Resumption and Cursor Greater Than Latest", "[server][sse_resume][empirical]") {
  TempTestDir temp_ws("sublift_sse_empirical_test");
  sublift::server::ServerConfig cfg;
  cfg.media_dir = temp_ws.path.string();
  sublift::server::HttpServer server(std::move(cfg));
  int port = server.bind_to_any_port("127.0.0.1");
  REQUIRE(port > 0);

  std::thread server_thread([&]() {
    server.listen_after_bind();
  });
  server.wait_until_ready();

  httplib::Client cli("127.0.0.1", port);
  cli.set_connection_timeout(std::chrono::seconds(2));
  cli.set_read_timeout(std::chrono::seconds(5));

  SECTION("Push 10 events, connect with Last-Event-ID: 4, verify exactly 5..10 received") {
    sublift::server::JobConfig jcfg{
        .video_path = (temp_ws.path / "test10.mp4").string(),
        .engine = "mock"
    };
    auto job = server.job_manager()->create_job(jcfg);
    REQUIRE(job != nullptr);

    // Push 10 events
    for (int i = 1; i <= 9; ++i) {
      job->event_stream->publish("progress", {{"step", i}, {"pct", i * 0.1}});
    }
    job->event_stream->publish("done", {{"ok", true}, {"total_entries", 0}, {"elapsed_ms", 500}});

    {
      std::lock_guard<std::mutex> lk(job->state_mutex);
      job->status = sublift::server::JobStatus::Completed;
    }

    httplib::Headers headers = {{"Last-Event-ID", "4"}};
    std::string response_body;
    auto res = cli.Get(("/api/jobs/" + job->job_id + "/events").c_str(), headers,
                       [&](const char* data, size_t data_length) {
                         response_body.append(data, data_length);
                         return true;
                       });

    REQUIRE(res != nullptr);
    REQUIRE(res->status == 200);

    // Verify 1..4 are NOT in response
    for (int i = 1; i <= 4; ++i) {
      REQUIRE(response_body.find("id: " + std::to_string(i) + "\n") == std::string::npos);
    }
    // Verify 5..10 ARE in response
    for (int i = 5; i <= 10; ++i) {
      REQUIRE(response_body.find("id: " + std::to_string(i) + "\n") != std::string::npos);
    }
  }

  SECTION("Push 10 events, connect with ?cursor=4, verify exactly 5..10 received") {
    sublift::server::JobConfig jcfg{
        .video_path = (temp_ws.path / "test10_cursor.mp4").string(),
        .engine = "mock"
    };
    auto job = server.job_manager()->create_job(jcfg);
    REQUIRE(job != nullptr);

    for (int i = 1; i <= 9; ++i) {
      job->event_stream->publish("progress", {{"step", i}, {"pct", i * 0.1}});
    }
    job->event_stream->publish("done", {{"ok", true}, {"total_entries", 0}, {"elapsed_ms", 500}});

    {
      std::lock_guard<std::mutex> lk(job->state_mutex);
      job->status = sublift::server::JobStatus::Completed;
    }

    std::string response_body;
    auto res = cli.Get(("/api/jobs/" + job->job_id + "/events?cursor=4").c_str(),
                       [&](const char* data, size_t data_length) {
                         response_body.append(data, data_length);
                         return true;
                       });

    REQUIRE(res != nullptr);
    REQUIRE(res->status == 200);

    for (int i = 1; i <= 4; ++i) {
      REQUIRE(response_body.find("id: " + std::to_string(i) + "\n") == std::string::npos);
    }
    for (int i = 5; i <= 10; ++i) {
      REQUIRE(response_body.find("id: " + std::to_string(i) + "\n") != std::string::npos);
    }
  }

  SECTION("Connect with cursor greater than latest event, stream waits for new events without replaying old ones") {
    std::filesystem::path test_mp4 = temp_ws.path / "test_wait.mp4";
    if (sublift::ffmpeg::available()) {
      std::string ffmpeg_bin = sublift::ffmpeg::resolve_ffmpeg_bin();
      std::string cmd = ffmpeg_bin + " -y -f lavfi -i testsrc=duration=0.5:size=128x128:rate=10 -pix_fmt yuv420p " + test_mp4.string() + " > /dev/null 2>&1";
      (void)std::system(cmd.c_str());
    } else {
      std::ofstream ofs(test_mp4, std::ios::binary);
      ofs << "dummy_video_payload";
    }

    sublift::server::JobConfig jcfg{
        .video_path = test_mp4.string(),
        .engine = "mock"
    };
    auto job = server.job_manager()->create_job(jcfg);
    REQUIRE(job != nullptr);

    // Wait for the background worker to finish the initial extraction
    int wait_c = 0;
    while (!job->is_terminal() && wait_c < 50) {
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      wait_c++;
    }

    // Now record the latest sequence ID produced so far
    auto initial_history = job->event_stream->get_all_history();
    REQUIRE(!initial_history.empty());
    std::uint64_t latest_seq = initial_history.back().seq_id;

    // Reset status to Running to simulate ongoing streaming work
    {
      std::lock_guard<std::mutex> lk(job->state_mutex);
      job->status = sublift::server::JobStatus::Running;
    }

    // Background thread that pushes new events (latest_seq + 1, latest_seq + 2, and done) after a short delay
    std::thread publisher_thread([job, latest_seq]() {
      std::this_thread::sleep_for(std::chrono::milliseconds(80));
      job->event_stream->publish("progress", {{"step", "resumed_1"}, {"pct", 0.85}});
      std::this_thread::sleep_for(std::chrono::milliseconds(80));
      job->event_stream->publish("progress", {{"step", "resumed_2"}, {"pct", 0.95}});
      std::this_thread::sleep_for(std::chrono::milliseconds(80));
      job->event_stream->publish("done", {{"ok", true}, {"total_entries", 0}, {"elapsed_ms", 600}});
      {
        std::lock_guard<std::mutex> lk(job->state_mutex);
        job->status = sublift::server::JobStatus::Completed;
      }
    });

    // Client connects with cursor = latest_seq (greater than or equal to previous events)
    std::string response_body;
    auto res = cli.Get(("/api/jobs/" + job->job_id + "/events?cursor=" + std::to_string(latest_seq)).c_str(),
                       [&](const char* data, size_t data_length) {
                         response_body.append(data, data_length);
                         return true;
                       });

    if (publisher_thread.joinable()) {
      publisher_thread.join();
    }

    REQUIRE(res != nullptr);
    REQUIRE(res->status == 200);

    // Verify previous initial events were NOT replayed
    for (std::uint64_t i = 1; i <= latest_seq; ++i) {
      REQUIRE(response_body.find("id: " + std::to_string(i) + "\n") == std::string::npos);
    }
    // Verify newly published events WERE received
    REQUIRE(response_body.find("id: " + std::to_string(latest_seq + 1) + "\n") != std::string::npos);
    REQUIRE(response_body.find("id: " + std::to_string(latest_seq + 2) + "\n") != std::string::npos);
    REQUIRE(response_body.find("id: " + std::to_string(latest_seq + 3) + "\n") != std::string::npos);
    REQUIRE(response_body.find("resumed_1") != std::string::npos);
    REQUIRE(response_body.find("resumed_2") != std::string::npos);
  }

  SECTION("Completed job with cursor greater than latest returns clean stream without old events") {
    sublift::server::JobConfig jcfg{
        .video_path = (temp_ws.path / "test_completed.mp4").string(),
        .engine = "mock"
    };
    auto job = server.job_manager()->create_job(jcfg);
    REQUIRE(job != nullptr);

    for (int i = 1; i <= 5; ++i) {
      job->event_stream->publish("progress", {{"step", i}, {"pct", i * 0.2}});
    }
    {
      std::lock_guard<std::mutex> lk(job->state_mutex);
      job->status = sublift::server::JobStatus::Completed;
    }

    // Connect with cursor = 10 (greater than latest seq_id = 5)
    std::string response_body;
    auto res = cli.Get(("/api/jobs/" + job->job_id + "/events?cursor=10").c_str(),
                       [&](const char* data, size_t data_length) {
                         response_body.append(data, data_length);
                         return true;
                       });

    REQUIRE(res != nullptr);
    REQUIRE(res->status == 200);
    // Should NOT contain any event ids 1..5
    for (int i = 1; i <= 5; ++i) {
      REQUIRE(response_body.find("id: " + std::to_string(i) + "\n") == std::string::npos);
    }
  }

  server.stop();
  if (server_thread.joinable()) {
    server_thread.join();
  }
}

TEST_CASE("Server Atomic Disk Save and Conflict Policies (Feature 12514)", "[server][save][export]") {
  TempTestDir temp_ws("sublift_ws_save_m4");
  std::error_code ec;

  // Create a synthetic media file inside workspace
  auto video_file = temp_ws.path / "sample_video.mp4";
  {
    std::ofstream ofs(video_file, std::ios::binary);
    ofs << "fake-mp4-data";
  }

  sublift::server::HttpServer server;
  server.workspace_manager()->set_media_directory(temp_ws.path.string());
  int port = server.bind_to_any_port("127.0.0.1");
  REQUIRE(port > 0);

  std::thread server_thread([&]() {
    server.listen_after_bind();
  });
  server.wait_until_ready();

  httplib::Client cli("127.0.0.1", port);
  cli.set_connection_timeout(std::chrono::seconds(2));
  cli.set_read_timeout(std::chrono::seconds(5));

  SECTION("Single job atomic save with temporary file, fsync, and atomic rename") {
    sublift::server::JobConfig cfg{
        .video_path = video_file.string(),
        .engine = "mock"
    };
    std::vector<sublift::SubtitleEntry> entries = {
        {.start_ms = 1000, .end_ms = 2500, .text = "First line of subtitles", .confidence = 0.95},
        {.start_ms = 3000, .end_ms = 5000, .text = "Second line of subtitles", .confidence = 0.98}
    };
    auto job = server.job_manager()->create_completed_job(cfg, std::move(entries));
    REQUIRE(job != nullptr);

    // Default target path: companion sample_video.srt
    auto companion_srt = temp_ws.path / "sample_video.srt";
    REQUIRE_FALSE(std::filesystem::exists(companion_srt, ec));

    auto res = cli.Post(("/api/jobs/" + job->job_id + "/save").c_str(), "{}", "application/json");
    REQUIRE(res != nullptr);
    REQUIRE(res->status == 200);

    auto res_json = nlohmann::json::parse(res->body);
    REQUIRE(res_json["status"] == "saved");
    REQUIRE(res_json["job_id"] == job->job_id);
    REQUIRE(res_json["empty_result"] == false);
    REQUIRE(std::filesystem::equivalent(std::filesystem::path(res_json["saved_path"].get<std::string>()), companion_srt));

    // Verify file exists on disk and content is valid SRT
    REQUIRE(std::filesystem::exists(companion_srt, ec));
    std::ifstream ifs(companion_srt);
    std::string srt_content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    REQUIRE(srt_content.find("First line of subtitles") != std::string::npos);
    REQUIRE(srt_content.find("Second line of subtitles") != std::string::npos);
    REQUIRE(srt_content.find("00:00:01,000 --> 00:00:02,500") != std::string::npos);
  }

  SECTION("Conflict policy 'skip' avoids overwriting existing file") {
    sublift::server::JobConfig cfg{
        .video_path = video_file.string(),
        .engine = "mock"
    };
    std::vector<sublift::SubtitleEntry> entries = {
        {.start_ms = 0, .end_ms = 1000, .text = "New subtitle data", .confidence = 1.0}
    };
    auto job = server.job_manager()->create_completed_job(cfg, std::move(entries));

    auto target_srt = temp_ws.path / "existing_skip.srt";
    {
      std::ofstream ofs(target_srt);
      ofs << "ORIGINAL UNTOUCHED CONTENT";
    }

    nlohmann::json body = {
        {"target_path", target_srt.string()},
        {"conflict_policy", "skip"}
    };

    auto res = cli.Post(("/api/jobs/" + job->job_id + "/save").c_str(), body.dump(), "application/json");
    REQUIRE(res != nullptr);
    REQUIRE(res->status == 200);

    auto res_json = nlohmann::json::parse(res->body);
    REQUIRE(res_json["status"] == "skipped");

    // Verify original content was untouched
    std::ifstream ifs(target_srt);
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    REQUIRE(content == "ORIGINAL UNTOUCHED CONTENT");
  }

  SECTION("Conflict policy 'deterministic_rename' automatically generates _1, _2 suffixes") {
    sublift::server::JobConfig cfg{
        .video_path = video_file.string(),
        .engine = "mock"
    };
    std::vector<sublift::SubtitleEntry> entries = {
        {.start_ms = 0, .end_ms = 1000, .text = "Renamed subtitle", .confidence = 1.0}
    };
    auto job = server.job_manager()->create_completed_job(cfg, std::move(entries));

    auto base_srt = temp_ws.path / "conflict_video.srt";
    auto rename_1_srt = temp_ws.path / "conflict_video_1.srt";
    auto rename_2_srt = temp_ws.path / "conflict_video_2.srt";

    // Pre-create conflict_video.srt and conflict_video_1.srt
    {
      std::ofstream ofs0(base_srt);
      ofs0 << "base";
      std::ofstream ofs1(rename_1_srt);
      ofs1 << "v1";
    }

    nlohmann::json body = {
        {"target_path", base_srt.string()},
        {"conflict_policy", "deterministic_rename"}
    };

    auto res = cli.Post(("/api/jobs/" + job->job_id + "/save").c_str(), body.dump(), "application/json");
    REQUIRE(res != nullptr);
    REQUIRE(res->status == 200);

    auto res_json = nlohmann::json::parse(res->body);
    REQUIRE(res_json["status"] == "saved");
    REQUIRE(std::filesystem::equivalent(std::filesystem::path(res_json["saved_path"].get<std::string>()), rename_2_srt));
    REQUIRE(std::filesystem::exists(rename_2_srt, ec));
  }

  SECTION("Conflict policy 'replace' atomically replaces existing file") {
    sublift::server::JobConfig cfg{
        .video_path = video_file.string(),
        .engine = "mock"
    };
    std::vector<sublift::SubtitleEntry> entries = {
        {.start_ms = 0, .end_ms = 1000, .text = "Replacement text", .confidence = 1.0}
    };
    auto job = server.job_manager()->create_completed_job(cfg, std::move(entries));

    auto target_srt = temp_ws.path / "replace_me.srt";
    {
      std::ofstream ofs(target_srt);
      ofs << "OLD DATA TO BE OVERWRITTEN";
    }

    nlohmann::json body = {
        {"target_path", target_srt.string()},
        {"conflict_policy", "replace"}
    };

    auto res = cli.Post(("/api/jobs/" + job->job_id + "/save").c_str(), body.dump(), "application/json");
    REQUIRE(res != nullptr);
    REQUIRE(res->status == 200);

    auto res_json = nlohmann::json::parse(res->body);
    REQUIRE(res_json["status"] == "saved");
    REQUIRE(std::filesystem::equivalent(std::filesystem::path(res_json["saved_path"].get<std::string>()), target_srt));

    std::ifstream ifs(target_srt);
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    REQUIRE(content.find("Replacement text") != std::string::npos);
    REQUIRE(content.find("OLD DATA") == std::string::npos);
  }

  SECTION("Empty subtitle protection: 0-entry jobs do not write empty file unless allow_empty is true") {
    sublift::server::JobConfig cfg{
        .video_path = video_file.string(),
        .engine = "mock"
    };
    auto job = server.job_manager()->create_completed_job(cfg, {});

    auto empty_target = temp_ws.path / "empty_result.srt";

    // 1. Default (allow_empty: false)
    nlohmann::json body1 = {
        {"target_path", empty_target.string()},
        {"allow_empty", false}
    };
    auto res1 = cli.Post(("/api/jobs/" + job->job_id + "/save").c_str(), body1.dump(), "application/json");
    REQUIRE(res1 != nullptr);
    REQUIRE(res1->status == 200);
    auto json1 = nlohmann::json::parse(res1->body);
    REQUIRE(json1["status"] == "empty_result");
    REQUIRE(json1["empty_result"] == true);
    REQUIRE_FALSE(std::filesystem::exists(empty_target, ec)); // File not written!

    // 2. Explicit (allow_empty: true)
    nlohmann::json body2 = {
        {"target_path", empty_target.string()},
        {"allow_empty", true}
    };
    auto res2 = cli.Post(("/api/jobs/" + job->job_id + "/save").c_str(), body2.dump(), "application/json");
    REQUIRE(res2 != nullptr);
    REQUIRE(res2->status == 200);
    auto json2 = nlohmann::json::parse(res2->body);
    REQUIRE(json2["status"] == "saved");
    REQUIRE(std::filesystem::exists(empty_target, ec)); // File written because requested
  }

  SECTION("Sandbox Security: Rejects directory traversal and out-of-bounds writes") {
    sublift::server::JobConfig cfg{
        .video_path = video_file.string(),
        .engine = "mock"
    };
    std::vector<sublift::SubtitleEntry> entries = {
        {.start_ms = 0, .end_ms = 1000, .text = "Test", .confidence = 1.0}
    };
    auto job = server.job_manager()->create_completed_job(cfg, std::move(entries));

    // Negative 1: Path traversal with .. escaping workspace
    nlohmann::json body_traversal = {
        {"target_path", (temp_ws.path / "../escaped.srt").string()}
    };
    auto res_trav = cli.Post(("/api/jobs/" + job->job_id + "/save").c_str(), body_traversal.dump(), "application/json");
    REQUIRE(res_trav != nullptr);
    REQUIRE(res_trav->status == 400);

    // Negative 2: Writing inside .sublift_cache
    nlohmann::json body_cache = {
        {"target_path", (temp_ws.path / ".sublift_cache" / "hacked.srt").string()}
    };
    auto res_cache = cli.Post(("/api/jobs/" + job->job_id + "/save").c_str(), body_cache.dump(), "application/json");
    REQUIRE(res_cache != nullptr);
    REQUIRE(res_cache->status == 400);

    // Negative 3: Invalid non-subtitle extension (.exe)
    nlohmann::json body_ext = {
        {"target_path", (temp_ws.path / "malicious.exe").string()}
    };
    auto res_ext = cli.Post(("/api/jobs/" + job->job_id + "/save").c_str(), body_ext.dump(), "application/json");
    REQUIRE(res_ext != nullptr);
    REQUIRE(res_ext->status == 400);
  }

  SECTION("POST /api/export/batch-save executes atomic batch save across multiple jobs") {
    auto vid1 = temp_ws.path / "batch_vid1.mp4";
    auto vid2 = temp_ws.path / "batch_vid2.mp4";
    {
      std::ofstream(vid1) << "1";
      std::ofstream(vid2) << "2";
    }

    std::vector<sublift::SubtitleEntry> entries1 = {
        {.start_ms = 0, .end_ms = 1000, .text = "Batch Sub 1", .confidence = 1.0}
    };
    std::vector<sublift::SubtitleEntry> entries2 = {
        {.start_ms = 0, .end_ms = 1000, .text = "Batch Sub 2", .confidence = 1.0}
    };
    auto job1 = server.job_manager()->create_completed_job({.video_path = vid1.string(), .engine = "mock"}, std::move(entries1));
    auto job2 = server.job_manager()->create_completed_job({.video_path = vid2.string(), .engine = "mock"}, std::move(entries2));

    nlohmann::json batch_req = {
        {"job_ids", {job1->job_id, job2->job_id}},
        {"conflict_policy", "deterministic_rename"}
    };

    auto res = cli.Post("/api/export/batch-save", batch_req.dump(), "application/json");
    REQUIRE(res != nullptr);
    REQUIRE(res->status == 200);

    auto batch_json = nlohmann::json::parse(res->body);
    REQUIRE(batch_json["total"] == 2);
    REQUIRE(batch_json["saved"] == 2);
    REQUIRE(batch_json["failed"] == 0);
    REQUIRE(batch_json["results"].is_array());
    REQUIRE(batch_json["results"].size() == 2);

    auto srt1 = temp_ws.path / "batch_vid1.srt";
    auto srt2 = temp_ws.path / "batch_vid2.srt";
    REQUIRE(std::filesystem::exists(srt1, ec));
    REQUIRE(std::filesystem::exists(srt2, ec));
  }

  SECTION("Error recovery: failed write or path violation leaves no residual .tmp files") {
    sublift::server::JobConfig cfg{
        .video_path = video_file.string(),
        .engine = "mock"
    };
    std::vector<sublift::SubtitleEntry> entries = {
        {.start_ms = 0, .end_ms = 1000, .text = "Error recovery test", .confidence = 1.0}
    };
    auto job = server.job_manager()->create_completed_job(cfg, std::move(entries));

    // 1. Attempt save to illegal path
    nlohmann::json bad_req = {{"target_path", (temp_ws.path / "../illegal.srt").string()}};
    auto res_bad = cli.Post(("/api/jobs/" + job->job_id + "/save").c_str(), bad_req.dump(), "application/json");
    REQUIRE(res_bad != nullptr);
    REQUIRE(res_bad->status == 400);

    // 2. Verify no temporary files (.*.tmp.*) exist in temp_ws
    for (const auto& entry : std::filesystem::directory_iterator(temp_ws.path)) {
      std::string filename = entry.path().filename().string();
      REQUIRE(filename.find(".tmp.") == std::string::npos);
    }
  }

  server.stop();
  if (server_thread.joinable()) {
    server_thread.join();
  }
}

TEST_CASE("Feature 12511: Unified Media Sandbox and Workspace Trust Boundary Matrix", "[server][sandbox][workspace][12511]") {
  TempTestDir ws1("sublift_ws1");
  TempTestDir ws2("sublift_ws2");
  TempTestDir outside_dir("sublift_outside");

  std::error_code ec;
  auto ws1_vid = ws1.path / "video1.mp4";
  auto ws2_vid = ws2.path / "video2.mp4";
  auto outside_vid = outside_dir.path / "outside.mp4";
  auto invalid_txt = ws1.path / "notes.txt";
  auto ts_code = ws1.path / "app.ts";

  if (sublift::ffmpeg::available()) {
    std::string ffmpeg_bin = sublift::ffmpeg::resolve_ffmpeg_bin();
    std::string cmd1 = ffmpeg_bin + " -y -f lavfi -i testsrc=duration=1.0:size=320x240:rate=10 -pix_fmt yuv420p " + ws1_vid.string() + " > /dev/null 2>&1";
    std::string cmd2 = ffmpeg_bin + " -y -f lavfi -i testsrc=duration=1.0:size=320x240:rate=10 -pix_fmt yuv420p " + ws2_vid.string() + " > /dev/null 2>&1";
    std::string cmd3 = ffmpeg_bin + " -y -f lavfi -i testsrc=duration=1.0:size=320x240:rate=10 -pix_fmt yuv420p " + outside_vid.string() + " > /dev/null 2>&1";
    (void)std::system(cmd1.c_str());
    (void)std::system(cmd2.c_str());
    (void)std::system(cmd3.c_str());
  } else {
    std::ofstream(ws1_vid) << "dummy mp4 content 1";
    std::ofstream(ws2_vid) << "dummy mp4 content 2";
    std::ofstream(outside_vid) << "dummy outside mp4";
  }
  std::ofstream(invalid_txt) << "some text notes";
  std::ofstream(ts_code) << "console.log('hello');";

  // Create an out-of-bounds symlink inside ws1 pointing to outside_vid
  auto symlink_escape = ws1.path / "symlink_escape.mp4";
  std::filesystem::create_symlink(outside_vid, symlink_escape, ec);

  // Create a sublift_cache internal file
  auto cache_file = ws1.path / ".sublift_cache" / "remux" / "cached.mp4";
  std::filesystem::create_directories(cache_file.parent_path(), ec);
  {
    std::ofstream(cache_file) << "internal cache";
  }

  sublift::server::ServerConfig config;
  config.media_dir = ws1.path.string();
  sublift::server::HttpServer server(std::move(config));

  int port = server.bind_to_any_port("127.0.0.1");
  REQUIRE(port > 0);

  std::thread server_thread([&]() {
    server.listen_after_bind();
  });
  server.wait_until_ready();

  httplib::Client cli("127.0.0.1", port);
  cli.set_connection_timeout(std::chrono::seconds(2));
  cli.set_read_timeout(std::chrono::seconds(5));

  SECTION("TC-SBX-01: Valid media in configured workspace succeeds across endpoints") {
    // 1. stream
    auto res_stream = cli.Get(("/api/video/stream?path=" + ws1_vid.string()).c_str());
    REQUIRE(res_stream != nullptr);
    REQUIRE(res_stream->status == 200);

    // 2. detect-region
    // engine=mock：本用例验证路径沙箱授权，而非引擎行为；显式固定引擎，
    // 避免测试机配置了 SUBLIFT_PADDLE_MODEL_DIR 时触发真实推理导致超时。
    nlohmann::json detect_body = {{"video_path", ws1_vid.string()}, {"engine", "mock"}};
    auto res_detect = cli.Post("/api/video/detect-region", detect_body.dump(), "application/json");
    REQUIRE(res_detect != nullptr);
    REQUIRE(res_detect->status == 200);

    // 3. jobs creation
    nlohmann::json job_body = {{"video_path", ws1_vid.string()}, {"engine", "mock"}};
    auto res_job = cli.Post("/api/jobs", job_body.dump(), "application/json");
    REQUIRE(res_job != nullptr);
    REQUIRE(res_job->status == 201);
  }

  SECTION("TC-SBX-02: Out-of-workspace absolute path rejected fail-closed") {
    // 1. stream
    auto res_stream = cli.Get(("/api/video/stream?path=" + outside_vid.string()).c_str());
    REQUIRE(res_stream != nullptr);
    REQUIRE(res_stream->status == 400);

    // 2. frame
    auto res_frame = cli.Get(("/api/video/frame?path=" + outside_vid.string()).c_str());
    REQUIRE(res_frame != nullptr);
    REQUIRE(res_frame->status == 400);

    // 3. detect-region
    nlohmann::json detect_body = {{"video_path", outside_vid.string()}};
    auto res_detect = cli.Post("/api/video/detect-region", detect_body.dump(), "application/json");
    REQUIRE(res_detect != nullptr);
    REQUIRE(res_detect->status == 400);

    // 4. jobs
    nlohmann::json job_body = {{"video_path", outside_vid.string()}, {"engine", "mock"}};
    auto res_job = cli.Post("/api/jobs", job_body.dump(), "application/json");
    REQUIRE(res_job != nullptr);
    REQUIRE(res_job->status == 400);
  }

  SECTION("TC-SBX-03: Relative path traversal .. escaping workspace rejected") {
    std::string escape_path = (ws1.path / "../" / outside_dir.path.filename() / "outside.mp4").string();

    auto res_stream = cli.Get(("/api/video/stream?path=" + escape_path).c_str());
    REQUIRE(res_stream != nullptr);
    REQUIRE(res_stream->status == 400);

    nlohmann::json job_body = {{"video_path", escape_path}, {"engine", "mock"}};
    auto res_job = cli.Post("/api/jobs", job_body.dump(), "application/json");
    REQUIRE(res_job != nullptr);
    REQUIRE(res_job->status == 400);
  }

  SECTION("TC-SBX-04: Symlink pointing outside workspace rejected") {
    auto res_stream = cli.Get(("/api/video/stream?path=" + symlink_escape.string()).c_str());
    REQUIRE(res_stream != nullptr);
    REQUIRE(res_stream->status == 400);

    nlohmann::json detect_body = {{"video_path", symlink_escape.string()}};
    auto res_detect = cli.Post("/api/video/detect-region", detect_body.dump(), "application/json");
    REQUIRE(res_detect != nullptr);
    REQUIRE(res_detect->status == 400);
  }

  SECTION("TC-SBX-05: Non-media extensions (.txt, .ts) rejected") {
    auto res_txt = cli.Get(("/api/video/stream?path=" + invalid_txt.string()).c_str());
    REQUIRE(res_txt != nullptr);
    REQUIRE(res_txt->status == 400);

    auto res_ts = cli.Get(("/api/video/stream?path=" + ts_code.string()).c_str());
    REQUIRE(res_ts != nullptr);
    REQUIRE(res_ts->status == 400);
  }

  SECTION("TC-SBX-06: Direct access to .sublift_cache internal assets rejected") {
    auto res_cache = cli.Get(("/api/video/stream?path=" + cache_file.string()).c_str());
    REQUIRE(res_cache != nullptr);
    REQUIRE(res_cache->status == 400);
  }

  SECTION("TC-SBX-07: Dynamic workspace switch immediately invalidates old root and authorizes new root") {
    // Before switch: ws1 is authorized, ws2 is unauthorized
    auto res_ws1_before = cli.Get(("/api/video/stream?path=" + ws1_vid.string()).c_str());
    REQUIRE(res_ws1_before->status == 200);

    auto res_ws2_before = cli.Get(("/api/video/stream?path=" + ws2_vid.string()).c_str());
    REQUIRE(res_ws2_before->status == 400);

    // Switch workspace to ws2
    nlohmann::json switch_body = {{"media_dir", ws2.path.string()}};
    auto res_switch = cli.Post("/api/config/workspace", switch_body.dump(), "application/json");
    REQUIRE(res_switch != nullptr);
    REQUIRE(res_switch->status == 200);

    // After switch: ws1 is now rejected (old root), ws2 is now authorized
    auto res_ws1_after = cli.Get(("/api/video/stream?path=" + ws1_vid.string()).c_str());
    REQUIRE(res_ws1_after->status == 400);

    auto res_ws2_after = cli.Get(("/api/video/stream?path=" + ws2_vid.string()).c_str());
    REQUIRE(res_ws2_after->status == 200);

    nlohmann::json job_ws2 = {{"video_path", ws2_vid.string()}, {"engine", "mock"}};
    auto res_job_ws2 = cli.Post("/api/jobs", job_ws2.dump(), "application/json");
    REQUIRE(res_job_ws2->status == 201);
  }

  SECTION("TC-SBX-08: Clearing workspace causes all media endpoints to fail-closed") {
    auto res_clear = cli.Post("/api/config/workspace/clear", "{}", "application/json");
    REQUIRE(res_clear != nullptr);
    REQUIRE(res_clear->status == 200);

    auto res_stream = cli.Get(("/api/video/stream?path=" + ws1_vid.string()).c_str());
    REQUIRE(res_stream != nullptr);
    REQUIRE(res_stream->status == 400);

    nlohmann::json job_body = {{"video_path", ws1_vid.string()}, {"engine", "mock"}};
    auto res_job = cli.Post("/api/jobs", job_body.dump(), "application/json");
    REQUIRE(res_job != nullptr);
    REQUIRE(res_job->status == 400);
  }

  server.stop();
  if (server_thread.joinable()) {
    server_thread.join();
  }
}

// ============================================================================
// ADR-0040 / Phase 14 D03：局域网自托管的访问控制合同
// ============================================================================

TEST_CASE("Loopback host detection covers IPv4, IPv6 and hostname forms", "[server][access_control]") {
  // IPv4 loopback：整个 127.0.0.0/8
  REQUIRE(sublift::server::is_loopback_host("127.0.0.1"));
  REQUIRE(sublift::server::is_loopback_host("127.8.15.200"));
  // IPv6 loopback 与常见书写变体
  REQUIRE(sublift::server::is_loopback_host("::1"));
  REQUIRE(sublift::server::is_loopback_host("[::1]"));
  REQUIRE(sublift::server::is_loopback_host("0:0:0:0:0:0:0:1"));
  // IPv4-mapped IPv6 loopback
  REQUIRE(sublift::server::is_loopback_host("::ffff:127.0.0.1"));
  // 主机名
  REQUIRE(sublift::server::is_loopback_host("localhost"));
  REQUIRE(sublift::server::is_loopback_host(" localhost "));

  // 非 loopback：LAN 地址、通配与欺诈形式
  REQUIRE_FALSE(sublift::server::is_loopback_host(""));
  REQUIRE_FALSE(sublift::server::is_loopback_host("0.0.0.0"));
  REQUIRE_FALSE(sublift::server::is_loopback_host("192.168.1.10"));
  REQUIRE_FALSE(sublift::server::is_loopback_host("::"));
  // "127." 前缀必须是点分十进制，避免把 "127.example.com" 误判为回环
  REQUIRE_FALSE(sublift::server::is_loopback_host("127.example.com"));
  REQUIRE_FALSE(sublift::server::is_loopback_host("128.0.0.1"));
}

TEST_CASE("Remote exposure validation rejects non-loopback without media root or lock", "[server][access_control]") {
  sublift::server::ServerConfig cfg;

  // loopback 一律放行，无需媒体根或锁定
  cfg.host = "127.0.0.1";
  REQUIRE(sublift::server::validate_remote_exposure(cfg, /*media_root_configured=*/false).empty());
  REQUIRE(sublift::server::validate_remote_exposure(cfg, /*media_root_configured=*/true).empty());

  // 非 loopback：无媒体根 → 拒绝；有媒体根但未锁定 → 拒绝；两者齐备 → 通过
  cfg.host = "0.0.0.0";
  auto err_no_media = sublift::server::validate_remote_exposure(cfg, false);
  REQUIRE_FALSE(err_no_media.empty());
  REQUIRE(err_no_media.find("媒体授权根") != std::string::npos);

  auto err_unlocked = sublift::server::validate_remote_exposure(cfg, true);
  REQUIRE_FALSE(err_unlocked.empty());
  REQUIRE(err_unlocked.find("锁定") != std::string::npos);

  cfg.workspace_locked = true;
  REQUIRE(sublift::server::validate_remote_exposure(cfg, true).empty());
}

TEST_CASE("Locked workspace rejects runtime modification and reports locked flag", "[server][workspace_lock]") {
  auto temp_dir = std::filesystem::temp_directory_path() /
                  ("sublift_wslock_" + std::to_string(::getpid()));
  std::error_code ec;
  std::filesystem::create_directories(temp_dir, ec);
  auto config_file = temp_dir / "cfg.json";

  sublift::server::ServerConfig cfg;
  cfg.port = 0;
  cfg.config_file = config_file.string();
  cfg.media_dir = temp_dir.string();
  cfg.workspace_locked = true;  // 启动期注入媒体根即锁定（容器/局域网部署形态）

  sublift::server::HttpServer server(std::move(cfg));
  int port = server.bind_to_any_port("127.0.0.1");
  REQUIRE(port > 0);

  std::thread server_thread([&server]() {
    server.listen_after_bind();
  });

  httplib::Client cli("127.0.0.1", port);
  cli.set_connection_timeout(2, 0);
  cli.set_read_timeout(5, 0);

  // 1. GET 报告 configured 且 locked
  auto res_get = cli.Get("/api/config/workspace");
  REQUIRE(res_get != nullptr);
  REQUIRE(res_get->status == 200);
  auto j_get = nlohmann::json::parse(res_get->body);
  REQUIRE(j_get["configured"].get<bool>());
  REQUIRE(j_get["locked"].get<bool>() == true);

  // 2. POST /api/config/workspace 被拒：403 且工作区保持不变
  auto res_post = cli.Post("/api/config/workspace",
                           R"({"media_dir":"/etc"})", "application/json");
  REQUIRE(res_post != nullptr);
  REQUIRE(res_post->status == 403);

  auto res_after = cli.Get("/api/config/workspace");
  REQUIRE(res_after != nullptr);
  auto j_after = nlohmann::json::parse(res_after->body);
  REQUIRE(j_after["configured"].get<bool>());
  REQUIRE(j_after["media_dir"].get<std::string>() ==
          std::filesystem::canonical(temp_dir).string());

  // 3. POST /api/config/workspace/clear 同合同被拒：403 且工作区保持不变
  auto res_clear = cli.Post("/api/config/workspace/clear", "{}", "application/json");
  REQUIRE(res_clear != nullptr);
  REQUIRE(res_clear->status == 403);
  auto j_clear = nlohmann::json::parse(res_clear->body);
  REQUIRE(j_clear["error"].get<std::string>() == "workspace_locked");

  // 4. DELETE /api/config/workspace 与 clear 共用受锁保护的处理路径
  auto res_delete = cli.Delete("/api/config/workspace");
  REQUIRE(res_delete != nullptr);
  REQUIRE(res_delete->status == 403);

  auto res_final = cli.Get("/api/config/workspace");
  REQUIRE(res_final != nullptr);
  auto j_final = nlohmann::json::parse(res_final->body);
  REQUIRE(j_final["configured"].get<bool>());
  REQUIRE(j_final["media_dir"].get<std::string>() ==
          std::filesystem::canonical(temp_dir).string());

  server.stop();
  if (server_thread.joinable()) {
    server_thread.join();
  }

  std::filesystem::remove_all(temp_dir, ec);
}

TEST_CASE("Access token enforces Bearer on API routes only", "[server][access_token]") {
  auto temp_dir = std::filesystem::temp_directory_path() /
                  ("sublift_token_" + std::to_string(::getpid()));
  std::error_code ec;
  std::filesystem::create_directories(temp_dir, ec);

  sublift::server::ServerConfig cfg;
  cfg.port = 0;
  cfg.access_token = "s3cret-token";
  cfg.media_dir = temp_dir.string();
  cfg.workspace_locked = true;
  // 最小静态目录：验证静态资源不要求 token（UI 对未持 token 的访问者可见）
  auto static_dir = temp_dir / "static";
  std::filesystem::create_directories(static_dir, ec);
  {
    std::ofstream ofs(static_dir / "index.html");
    ofs << "<!doctype html><html><body>sublift</body></html>";
  }
  cfg.static_dir = static_dir.string();

  sublift::server::HttpServer server(std::move(cfg));
  int port = server.bind_to_any_port("127.0.0.1");
  REQUIRE(port > 0);

  std::thread server_thread([&server]() {
    server.listen_after_bind();
  });

  httplib::Client cli("127.0.0.1", port);
  cli.set_connection_timeout(2, 0);
  cli.set_read_timeout(5, 0);

  // 1. 无 token / 错误 token / 非 Bearer scheme → 401
  auto res_missing = cli.Get("/api/system/info");
  REQUIRE(res_missing != nullptr);
  REQUIRE(res_missing->status == 401);
  REQUIRE(res_missing->get_header_value("WWW-Authenticate") == "Bearer");

  auto res_wrong = cli.Get("/api/system/info",
                           {{"Authorization", "Bearer wrong-token"}});
  REQUIRE(res_wrong != nullptr);
  REQUIRE(res_wrong->status == 401);

  auto res_scheme = cli.Get("/api/system/info",
                            {{"Authorization", "Basic s3cret-token"}});
  REQUIRE(res_scheme != nullptr);
  REQUIRE(res_scheme->status == 401);

  // 2. 正确 token → 200
  auto res_ok = cli.Get("/api/system/info",
                        {{"Authorization", "Bearer s3cret-token"}});
  REQUIRE(res_ok != nullptr);
  REQUIRE(res_ok->status == 200);

  // 3. 静态资源不要求 token（未持 token 的访问者得到 UI 而非空白页）
  auto res_static = cli.Get("/");
  REQUIRE(res_static != nullptr);
  REQUIRE(res_static->status == 200);

  server.stop();
  if (server_thread.joinable()) {
    server_thread.join();
  }

  std::filesystem::remove_all(temp_dir, ec);
}


