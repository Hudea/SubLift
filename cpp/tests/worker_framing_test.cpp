#include <arpa/inet.h>
#include <catch2/catch_test_macros.hpp>
#include <cerrno>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

#include "framing.hpp"

using namespace sublift::ipc;

TEST_CASE("Framed message framing round-trip and error cases", "[worker][ipc][framing]") {
  int fds[2];
  REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

  int rfd = fds[0];
  int wfd = fds[1];

  SECTION("Normal message round-trip") {
    std::string test_payload = R"({"type":"hello","protocol_version":1,"runtime":"cpp"})";
    REQUIRE(write_framed_message(wfd, test_payload) == FramingStatus::Success);

    auto res = read_framed_message(rfd);
    REQUIRE(res.status == FramingStatus::Success);
    REQUIRE(res.payload == test_payload);
  }

  SECTION("Multiple messages stream round-trip") {
    std::vector<std::string> messages = {
        R"({"type":"hello"})",
        R"({"type":"start_job","mode":"path","video_path":"/tmp/test.mp4"})",
        R"({"type":"cancel_job"})",
        R"({"type":"bye"})",
    };

    for (const auto& msg : messages) {
      REQUIRE(write_framed_message(wfd, msg) == FramingStatus::Success);
    }

    for (const auto& msg : messages) {
      auto res = read_framed_message(rfd);
      REQUIRE(res.status == FramingStatus::Success);
      REQUIRE(res.payload == msg);
    }
  }

  SECTION("Writing empty payload returns ZeroLength") {
    REQUIRE(write_framed_message(wfd, "") == FramingStatus::ZeroLength);
  }

  SECTION("Writing payload exceeding 64MB returns MessageTooLarge") {
    // Write call with large length (we test length validation before buffer write)
    std::string huge_view_dummy;
    // std::string_view with size > 64MB
    std::string_view huge_view("a", 64 * 1024 * 1024 + 1);
    REQUIRE(write_framed_message(wfd, huge_view) == FramingStatus::MessageTooLarge);
  }

  SECTION("Clean EOF when connection closes with 0 header bytes") {
    close(wfd);
    wfd = -1;

    auto res = read_framed_message(rfd);
    REQUIRE(res.status == FramingStatus::Eof);
  }

  SECTION("Zero length header returns ZeroLength error") {
    std::uint32_t net_len = htonl(0);
    REQUIRE(write_n(wfd, &net_len, 4) == 4);

    auto res = read_framed_message(rfd);
    REQUIRE(res.status == FramingStatus::ZeroLength);
  }

  SECTION("Message too large (>64MB) returns MessageTooLarge without allocation") {
    std::uint32_t net_len = htonl(65 * 1024 * 1024);
    REQUIRE(write_n(wfd, &net_len, 4) == 4);

    auto res = read_framed_message(rfd);
    REQUIRE(res.status == FramingStatus::MessageTooLarge);
  }

  SECTION("Incomplete header on premature disconnect") {
    std::uint8_t partial_header[2] = {0, 0};
    REQUIRE(write_n(wfd, partial_header, 2) == 2);
    close(wfd);
    wfd = -1;

    auto res = read_framed_message(rfd);
    REQUIRE(res.status == FramingStatus::IncompleteHeader);
  }

  SECTION("Incomplete body on premature disconnect") {
    std::uint32_t net_len = htonl(100);
    REQUIRE(write_n(wfd, &net_len, 4) == 4);
    std::string partial_body(30, 'a');
    REQUIRE(write_n(wfd, partial_body.data(), 30) == 30);
    close(wfd);
    wfd = -1;

    auto res = read_framed_message(rfd);
    REQUIRE(res.status == FramingStatus::IncompleteBody);
  }

  SECTION("Invalid file descriptor returns IoError with EBADF") {
    auto read_res = read_framed_message(-1);
    REQUIRE(read_res.status == FramingStatus::IoError);
    REQUIRE(read_res.error_code == EBADF);

    auto write_res = write_framed_message(-1, "test");
    REQUIRE(write_res == FramingStatus::IoError);
  }

  if (rfd >= 0) close(rfd);
  if (wfd >= 0) close(wfd);
}
