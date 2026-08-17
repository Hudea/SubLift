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
  cli.set_read_timeout(std::chrono::seconds(2));

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
  cli.set_read_timeout(std::chrono::seconds(2));

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

  sublift::server::HttpServer server;
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
  sublift::server::HttpServer server;
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
  sublift::server::HttpServer server;
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

      // Verify SRT Export endpoint
      auto exp_res = cli.Get(("/api/jobs/" + job_id + "/export").c_str());
      REQUIRE(exp_res != nullptr);
      if (job_ctx->status == sublift::server::JobStatus::Completed) {
        REQUIRE(exp_res->status == 200);
        REQUIRE(exp_res->get_header_value("Content-Type") == "text/plain; charset=utf-8");
        REQUIRE(exp_res->get_header_value("Content-Disposition").find("attachment;") != std::string::npos);
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
  sublift::server::HttpServer server;
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


