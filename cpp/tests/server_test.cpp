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
