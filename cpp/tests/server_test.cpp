#include <catch2/catch_test_macros.hpp>

#include <chrono>
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

  SECTION("Nonexistent path returns 404 Not Found") {
    auto res = cli.Get("/api/video/stream?path=/nonexistent/path/test.mp4");
    REQUIRE(res != nullptr);
    REQUIRE(res->status == 404);
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

  SECTION("Nonexistent video returns 404 Not Found") {
    auto res = cli.Get("/api/video/frame?path=/nonexistent/dummy.mp4");
    REQUIRE(res != nullptr);
    REQUIRE(res->status == 404);
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

    // Non-existent video file
    nlohmann::json req_nonexist = {{"video_path", "/nonexistent/video.mp4"}};
    auto res3 = cli.Post("/api/jobs", req_nonexist.dump(), "application/json");
    REQUIRE(res3 != nullptr);
    REQUIRE(res3->status == 404);
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
