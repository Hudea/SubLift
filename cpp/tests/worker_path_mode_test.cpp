#include <sys/socket.h>
#include <unistd.h>
#include <catch2/catch_test_macros.hpp>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include "connection.hpp"
#include "engine_factory.hpp"
#include "framing.hpp"
#include "protocol.hpp"

using namespace sublift::ipc;
using namespace sublift::worker;

namespace {

std::string create_synthetic_test_video() {
  std::string video_path = (std::filesystem::temp_directory_path() / "sublift_test_worker_video.mp4").string();
  if (std::filesystem::exists(video_path)) {
    return video_path;
  }

  std::string cmd = "ffmpeg -y -f lavfi -i testsrc=size=320x240:rate=1 -t 3 -pix_fmt yuv420p " + video_path + " >/dev/null 2>&1";
  std::system(cmd.c_str());
  return video_path;
}

}  // namespace

TEST_CASE("Worker Connection and Path Mode Mock integration test", "[worker][ipc][path_mode]") {
  int fds[2];
  REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

  int client_fd = fds[0];
  int worker_fd = fds[1];

  EngineFactory factory("mock");

  // Run WorkerConnection in background thread
  std::thread worker_thread([worker_fd, factory]() {
    WorkerConnection conn(worker_fd, factory);
    conn.run();
  });

  SECTION("Handshake hello -> bye capability response") {
    HelloMsg hello{.client = "test_client", .protocol_version = 1};
    REQUIRE(write_framed_message(client_fd, serialize_message(hello)) == FramingStatus::Success);

    auto res = read_framed_message(client_fd);
    REQUIRE(res.status == FramingStatus::Success);

    Message msg = parse_message(res.payload);
    REQUIRE(std::holds_alternative<ByeMsg>(msg));
    const auto& bye = std::get<ByeMsg>(msg);
    REQUIRE(bye.runtime == "cpp");
    REQUIRE(std::find(bye.engines.begin(), bye.engines.end(), "mock") != bye.engines.end());
    REQUIRE(std::find(bye.engines.begin(), bye.engines.end(), "paddle") == bye.engines.end());
  }

  SECTION("Engine mismatch fails with DoneMsg ok=false") {
    StartJobMsg start{
        .video_id = "v_mismatch",
        .fps = 1.0,
        .engine = "vision",  // Server bound to mock!
        .confidence_threshold = 0.5,
        .video_path = "/tmp/fake.mp4",
    };

    REQUIRE(write_framed_message(client_fd, serialize_message(start)) == FramingStatus::Success);

    auto res = read_framed_message(client_fd);
    REQUIRE(res.status == FramingStatus::Success);

    Message msg = parse_message(res.payload);
    REQUIRE(std::holds_alternative<DoneMsg>(msg));
    const auto& done = std::get<DoneMsg>(msg);
    REQUIRE(done.ok == false);
    REQUIRE(done.error.has_value());
    REQUIRE(done.error->find("engine 不匹配") != std::string::npos);
  }

  SECTION("Paddle engine request fails with DoneMsg ok=false") {
    StartJobMsg start{
        .video_id = "v_paddle",
        .fps = 1.0,
        .engine = "paddle",
        .confidence_threshold = 0.5,
        .video_path = "/tmp/fake.mp4",
    };

    REQUIRE(write_framed_message(client_fd, serialize_message(start)) == FramingStatus::Success);

    auto res = read_framed_message(client_fd);
    REQUIRE(res.status == FramingStatus::Success);

    Message msg = parse_message(res.payload);
    REQUIRE(std::holds_alternative<DoneMsg>(msg));
    const auto& done = std::get<DoneMsg>(msg);
    REQUIRE(done.ok == false);
    REQUIRE(done.error.has_value());
    REQUIRE(done.error->find("paddle 引擎不支持") != std::string::npos);
  }

  SECTION("Non-existent video file fails with DoneMsg ok=false") {
    StartJobMsg start{
        .video_id = "v_missing",
        .fps = 1.0,
        .engine = "mock",
        .confidence_threshold = 0.5,
        .video_path = "/non_existent_file_path_12345.mp4",
    };

    REQUIRE(write_framed_message(client_fd, serialize_message(start)) == FramingStatus::Success);

    auto res = read_framed_message(client_fd);
    REQUIRE(res.status == FramingStatus::Success);

    Message msg = parse_message(res.payload);
    REQUIRE(std::holds_alternative<DoneMsg>(msg));
    const auto& done = std::get<DoneMsg>(msg);
    REQUIRE(done.ok == false);
    REQUIRE(done.error.has_value());
    REQUIRE(done.error->find("视频文件不存在") != std::string::npos);
  }

  SECTION("Path Mode Mock execution over synthetic video") {
    std::string video_path = create_synthetic_test_video();
    REQUIRE(std::filesystem::exists(video_path));

    StartJobMsg start{
        .video_id = "v_path_mock",
        .fps = 1.0,
        .engine = "mock",
        .confidence_threshold = 0.5,
        .video_path = video_path,
    };

    REQUIRE(write_framed_message(client_fd, serialize_message(start)) == FramingStatus::Success);

    std::vector<Message> received_messages;
    bool done_received = false;

    while (!done_received) {
      auto res = read_framed_message(client_fd);
      REQUIRE(res.status == FramingStatus::Success);

      Message msg = parse_message(res.payload);
      received_messages.push_back(msg);

      if (std::holds_alternative<DoneMsg>(msg)) {
        done_received = true;
        const auto& done = std::get<DoneMsg>(msg);
        REQUIRE(done.ok == true);
      }
    }

    // Verify message sequence
    REQUIRE(!received_messages.empty());
    REQUIRE(std::holds_alternative<ProgressMsg>(received_messages.front()));
    REQUIRE(std::get<ProgressMsg>(received_messages.front()).stage == "ready");

    REQUIRE(std::holds_alternative<DoneMsg>(received_messages.back()));

    // Verify EntriesMsg present
    bool has_entries = false;
    for (const auto& m : received_messages) {
      if (std::holds_alternative<EntriesMsg>(m)) {
        has_entries = true;
        const auto& entries_msg = std::get<EntriesMsg>(m);
        REQUIRE(entries_msg.video_id == "v_path_mock");
        REQUIRE(entries_msg.is_final == true);
      }
    }
    REQUIRE(has_entries);
  }

  SECTION("CancelJobMsg cancels running job cleanly") {
    std::string video_path = create_synthetic_test_video();
    REQUIRE(std::filesystem::exists(video_path));

    StartJobMsg start{
        .video_id = "v_cancel",
        .fps = 1.0,
        .engine = "mock",
        .confidence_threshold = 0.5,
        .video_path = video_path,
    };

    REQUIRE(write_framed_message(client_fd, serialize_message(start)) == FramingStatus::Success);

    // Immediately send cancel_job
    CancelJobMsg cancel{.video_id = "v_cancel"};
    REQUIRE(write_framed_message(client_fd, serialize_message(cancel)) == FramingStatus::Success);

    bool cancel_done = false;
    while (!cancel_done) {
      auto res = read_framed_message(client_fd);
      if (res.status != FramingStatus::Success) break;

      Message msg = parse_message(res.payload);
      if (std::holds_alternative<DoneMsg>(msg)) {
        cancel_done = true;
      }
    }
    REQUIRE(cancel_done);
  }

  SECTION("Second start_job while path job running returns DoneMsg 已有 Job") {
    std::string video_path = create_synthetic_test_video();
    REQUIRE(std::filesystem::exists(video_path));

    StartJobMsg start1{
        .video_id = "v_first_job",
        .fps = 1.0,
        .engine = "mock",
        .confidence_threshold = 0.5,
        .video_path = video_path,
    };
    REQUIRE(write_framed_message(client_fd, serialize_message(start1)) == FramingStatus::Success);

    // Wait for ready so job is marked running.
    auto res_ready = read_framed_message(client_fd);
    REQUIRE(res_ready.status == FramingStatus::Success);
    Message ready_msg = parse_message(res_ready.payload);
    REQUIRE(std::holds_alternative<ProgressMsg>(ready_msg));
    REQUIRE(std::get<ProgressMsg>(ready_msg).stage == "ready");

    StartJobMsg start2{
        .video_id = "v_second_job",
        .fps = 1.0,
        .engine = "mock",
        .confidence_threshold = 0.5,
        .video_path = video_path,
    };
    REQUIRE(write_framed_message(client_fd, serialize_message(start2)) == FramingStatus::Success);

    // Drain until we observe the rejection for the concurrent start (may interleave
    // with first-job progress/push). Must arrive before first job finishes.
    bool saw_busy_reject = false;
    bool first_done = false;
    while (!saw_busy_reject && !first_done) {
      auto res = read_framed_message(client_fd);
      REQUIRE(res.status == FramingStatus::Success);
      Message msg = parse_message(res.payload);
      if (std::holds_alternative<DoneMsg>(msg)) {
        const auto& done = std::get<DoneMsg>(msg);
        if (done.video_id == "v_second_job") {
          REQUIRE(done.ok == false);
          REQUIRE(done.error.has_value());
          REQUIRE(done.error->find("已有 Job") != std::string::npos);
          saw_busy_reject = true;
        } else if (done.video_id == "v_first_job") {
          first_done = true;
        }
      }
    }
    REQUIRE(saw_busy_reject);
    REQUIRE_FALSE(first_done);

    // Let first job finish (or cancel) so connection teardown is clean.
    while (!first_done) {
      auto res = read_framed_message(client_fd);
      if (res.status != FramingStatus::Success) break;
      Message msg = parse_message(res.payload);
      if (std::holds_alternative<DoneMsg>(msg) &&
          std::get<DoneMsg>(msg).video_id == "v_first_job") {
        first_done = true;
      }
    }
  }
#if defined(SUBLIFT_ENABLE_VISION) && SUBLIFT_ENABLE_VISION
  SECTION("Vision engine path mode execution when enabled") {
    if (sublift::is_vision_available()) {
      int vision_fds[2];
      REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, vision_fds) == 0);
      int v_client = vision_fds[0];
      int v_worker = vision_fds[1];

      EngineFactory vision_factory("vision");
      std::thread v_thread([v_worker, vision_factory]() {
        WorkerConnection conn(v_worker, vision_factory);
        conn.run();
      });

      std::string video_path = create_synthetic_test_video();
      StartJobMsg start{
          .video_id = "v_vision_mode",
          .fps = 1.0,
          .engine = "vision",
          .confidence_threshold = 0.5,
          .video_path = video_path,
      };

      REQUIRE(write_framed_message(v_client, serialize_message(start)) == FramingStatus::Success);

      bool vision_done = false;
      while (!vision_done) {
        auto res = read_framed_message(v_client);
        REQUIRE(res.status == FramingStatus::Success);
        Message msg = parse_message(res.payload);
        if (std::holds_alternative<DoneMsg>(msg)) {
          vision_done = true;
          REQUIRE(std::get<DoneMsg>(msg).ok == true);
        }
      }

      close(v_client);
      if (v_thread.joinable()) v_thread.join();
    }
  }
#endif

  // Close client socket to trigger worker thread termination
  close(client_fd);

  if (worker_thread.joinable()) {
    worker_thread.join();
  }
}
