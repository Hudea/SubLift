#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <chrono>
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
  std::string video_path =
      (std::filesystem::temp_directory_path() / "sublift_test_worker_cancel_video.mp4").string();
  if (std::filesystem::exists(video_path)) {
    return video_path;
  }

  std::string cmd =
      "ffmpeg -y -f lavfi -i testsrc=size=320x240:rate=2 -t 5 -pix_fmt yuv420p " + video_path +
      " >/dev/null 2>&1";
  std::system(cmd.c_str());
  return video_path;
}

// 8x6 solid red JPEG via ffmpeg lavfi (FF D8 magic). Distinct from legacy
// solid-fill 320x240; ImageIO must decode or frame path fails closed.
constexpr const char* kMinimalJpegB64 =
    "/9j/4AAQSkZJRgABAgAAAQABAAD//gAQTGF2YzYyLjI4LjEwMAD/2wBDAAgEBAQEBAUFBQUFBQYG"
    "BgYGBgYGBgYGBgYHBwcICAgHBwcGBgcHCAgICAkJCQgICAgJCQoKCgwMCwsODg4RERT/xABMAAEB"
    "AAAAAAAAAAAAAAAAAAAABgEBAQAAAAAAAAAAAAAAAAAABgcQAQAAAAAAAAAAAAAAAAAAAAARAQ"
    "AAAAAAAAAAAAAAAAAAAAD/wAARCAAGAAgDASIAAhEAAxEA/9oADAMBAAIRAxEAPwCLAE1/f//Z";

bool drain_until_done_cancelled(int client_fd, bool* saw_push, bool* saw_entries,
                                bool* saw_success_done) {
  *saw_push = false;
  *saw_entries = false;
  *saw_success_done = false;
  while (true) {
    auto res = read_framed_message(client_fd);
    if (res.status != FramingStatus::Success) {
      return false;
    }
    Message msg = parse_message(res.payload);
    if (std::holds_alternative<PushEntryMsg>(msg)) {
      *saw_push = true;
    } else if (std::holds_alternative<EntriesMsg>(msg)) {
      *saw_entries = true;
    } else if (std::holds_alternative<DoneMsg>(msg)) {
      const auto& done = std::get<DoneMsg>(msg);
      if (done.ok) {
        *saw_success_done = true;
      } else if (done.error.has_value() && *done.error == "cancelled") {
        return true;
      }
    }
  }
}

}  // namespace

TEST_CASE("Worker Cancel suppresses late push_entry", "[worker][ipc][cancel]") {
  int fds[2];
  REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

  int client_fd = fds[0];
  int worker_fd = fds[1];

  EngineFactory factory("mock");

  std::thread worker_thread([worker_fd, factory]() {
    WorkerConnection conn(worker_fd, factory);
    conn.run();
  });

  std::string video_path = create_synthetic_test_video();
  REQUIRE(std::filesystem::exists(video_path));

  StartJobMsg start{
      .video_id = "v_cancel_test",
      .fps = 2.0,
      .engine = "mock",
      .confidence_threshold = 0.5,
      .video_path = video_path,
  };

  REQUIRE(write_framed_message(client_fd, serialize_message(start)) == FramingStatus::Success);

  auto res1 = read_framed_message(client_fd);
  REQUIRE(res1.status == FramingStatus::Success);
  Message ready_msg = parse_message(res1.payload);
  REQUIRE(std::holds_alternative<ProgressMsg>(ready_msg));

  CancelJobMsg cancel{.video_id = "v_cancel_test"};
  REQUIRE(write_framed_message(client_fd, serialize_message(cancel)) == FramingStatus::Success);

  bool saw_push = false;
  bool saw_entries = false;
  bool saw_success_done = false;
  REQUIRE(drain_until_done_cancelled(client_fd, &saw_push, &saw_entries, &saw_success_done));
  REQUIRE_FALSE(saw_push);
  REQUIRE_FALSE(saw_entries);
  REQUIRE_FALSE(saw_success_done);

  // After DoneMsg(cancelled), cancel_job has joined the path thread; no late
  // push_entry/entries/success-done may still be in flight.
  REQUIRE(fcntl(client_fd, F_SETFL, O_NONBLOCK) == 0);
  auto late = read_framed_message(client_fd);
  if (late.status == FramingStatus::Success) {
    Message late_msg = parse_message(late.payload);
    REQUIRE_FALSE(std::holds_alternative<PushEntryMsg>(late_msg));
    REQUIRE_FALSE(std::holds_alternative<EntriesMsg>(late_msg));
    if (std::holds_alternative<DoneMsg>(late_msg)) {
      REQUIRE(std::get<DoneMsg>(late_msg).ok == false);
    }
  }

  close(client_fd);
  if (worker_thread.joinable()) {
    worker_thread.join();
  }
}

TEST_CASE("Worker client disconnect mid path job reclaims resources",
          "[worker][ipc][cancel][disconnect]") {
  int fds[2];
  REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

  int client_fd = fds[0];
  int worker_fd = fds[1];

  EngineFactory factory("mock");
  std::atomic<bool> worker_exited{false};

  std::thread worker_thread([worker_fd, factory, &worker_exited]() {
    WorkerConnection conn(worker_fd, factory);
    conn.run();
    worker_exited.store(true);
  });

  std::string video_path = create_synthetic_test_video();
  REQUIRE(std::filesystem::exists(video_path));

  StartJobMsg start{
      .video_id = "v_disconnect",
      .fps = 2.0,
      .engine = "mock",
      .confidence_threshold = 0.5,
      .video_path = video_path,
  };
  REQUIRE(write_framed_message(client_fd, serialize_message(start)) == FramingStatus::Success);

  auto res_ready = read_framed_message(client_fd);
  REQUIRE(res_ready.status == FramingStatus::Success);

  // Client disconnect ≈ cancel + finally reclaim (connection EOF → cancel_job).
  close(client_fd);

  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
  while (!worker_exited.load() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  REQUIRE(worker_exited.load());

  if (worker_thread.joinable()) {
    worker_thread.join();
  }
}

TEST_CASE("Worker same-connection restart after cancel has no video_id crosstalk",
          "[worker][ipc][cancel][restart]") {
  int fds[2];
  REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

  int client_fd = fds[0];
  int worker_fd = fds[1];

  EngineFactory factory("mock");

  std::thread worker_thread([worker_fd, factory]() {
    WorkerConnection conn(worker_fd, factory);
    conn.run();
  });

  std::string video_path = create_synthetic_test_video();
  REQUIRE(std::filesystem::exists(video_path));

  // Job 1: start then cancel
  StartJobMsg start1{
      .video_id = "v_old_job",
      .fps = 2.0,
      .engine = "mock",
      .confidence_threshold = 0.5,
      .video_path = video_path,
  };
  REQUIRE(write_framed_message(client_fd, serialize_message(start1)) == FramingStatus::Success);

  auto res1 = read_framed_message(client_fd);
  REQUIRE(res1.status == FramingStatus::Success);

  CancelJobMsg cancel{.video_id = "v_old_job"};
  REQUIRE(write_framed_message(client_fd, serialize_message(cancel)) == FramingStatus::Success);

  bool saw_push = false;
  bool saw_entries = false;
  bool saw_success_done = false;
  REQUIRE(drain_until_done_cancelled(client_fd, &saw_push, &saw_entries, &saw_success_done));

  // Job 2: new video_id on same connection
  StartJobMsg start2{
      .video_id = "v_new_job",
      .fps = 1.0,
      .engine = "mock",
      .confidence_threshold = 0.5,
      .video_path = video_path,
  };
  REQUIRE(write_framed_message(client_fd, serialize_message(start2)) == FramingStatus::Success);

  bool done2 = false;
  std::vector<std::string> video_ids_after_restart;
  while (!done2) {
    auto res = read_framed_message(client_fd);
    REQUIRE(res.status == FramingStatus::Success);
    Message msg = parse_message(res.payload);

    if (std::holds_alternative<ProgressMsg>(msg)) {
      video_ids_after_restart.push_back(std::get<ProgressMsg>(msg).video_id);
    } else if (std::holds_alternative<PushEntryMsg>(msg)) {
      video_ids_after_restart.push_back(std::get<PushEntryMsg>(msg).video_id);
    } else if (std::holds_alternative<EntriesMsg>(msg)) {
      video_ids_after_restart.push_back(std::get<EntriesMsg>(msg).video_id);
    } else if (std::holds_alternative<DoneMsg>(msg)) {
      const auto& done = std::get<DoneMsg>(msg);
      video_ids_after_restart.push_back(done.video_id);
      REQUIRE(done.ok == true);
      REQUIRE(done.video_id == "v_new_job");
      done2 = true;
    } else if (std::holds_alternative<ErrorMsg>(msg)) {
      FAIL("unexpected error after restart: " << std::get<ErrorMsg>(msg).message);
    }
  }

  REQUIRE_FALSE(video_ids_after_restart.empty());
  for (const auto& vid : video_ids_after_restart) {
    REQUIRE(vid == "v_new_job");
    REQUIRE(vid != "v_old_job");
  }

  close(client_fd);
  if (worker_thread.joinable()) {
    worker_thread.join();
  }
}

TEST_CASE("Worker Frame Mode lifecycle with real JPEG", "[worker][ipc][frame_mode]") {
  int fds[2];
  REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

  int client_fd = fds[0];
  int worker_fd = fds[1];

  EngineFactory factory("mock");

  std::thread worker_thread([worker_fd, factory]() {
    WorkerConnection conn(worker_fd, factory);
    conn.run();
  });

  // region_box matches 8x6 fixture (ImageIO decode dims). Oversized boxes would
  // fail ROI on real JPEG but would have "worked" with the old solid-fill path.
  StartJobMsg start{
      .video_id = "v_frame_mode",
      .fps = 1.0,
      .engine = "mock",
      .confidence_threshold = 0.5,
      .region_box = sublift::Box2i{0, 0, 8, 6},
  };

  REQUIRE(write_framed_message(client_fd, serialize_message(start)) == FramingStatus::Success);

  auto res_ready = read_framed_message(client_fd);
  REQUIRE(res_ready.status == FramingStatus::Success);
  Message ready_msg = parse_message(res_ready.payload);
  REQUIRE(std::holds_alternative<ProgressMsg>(ready_msg));
  REQUIRE(std::get<ProgressMsg>(ready_msg).stage == "ready");

  FrameMsg frame_msg{
      .video_id = "v_frame_mode",
      .ts_ms = 0,
      .jpeg_bytes = kMinimalJpegB64,
  };

  REQUIRE(write_framed_message(client_fd, serialize_message(frame_msg)) == FramingStatus::Success);

  auto res_frame = read_framed_message(client_fd);
  REQUIRE(res_frame.status == FramingStatus::Success);
  Message frame_resp = parse_message(res_frame.payload);
  // Real JPEG must decode to 8x6; solid-fill fallback is gone.
  if (std::holds_alternative<ErrorMsg>(frame_resp)) {
    FAIL("frame failed: " << std::get<ErrorMsg>(frame_resp).message);
  }
  REQUIRE(std::holds_alternative<ProgressMsg>(frame_resp));
  REQUIRE(std::get<ProgressMsg>(frame_resp).stage == "processing");

  FinalizeMsg finalize_msg{.video_id = "v_frame_mode"};
  REQUIRE(write_framed_message(client_fd, serialize_message(finalize_msg)) == FramingStatus::Success);

  bool received_entries = false;
  bool received_done = false;

  while (!received_done) {
    auto res = read_framed_message(client_fd);
    REQUIRE(res.status == FramingStatus::Success);
    Message msg = parse_message(res.payload);

    if (std::holds_alternative<EntriesMsg>(msg)) {
      received_entries = true;
      const auto& entries_msg = std::get<EntriesMsg>(msg);
      REQUIRE(entries_msg.video_id == "v_frame_mode");
      REQUIRE(entries_msg.is_final == true);
    } else if (std::holds_alternative<DoneMsg>(msg)) {
      received_done = true;
      const auto& done = std::get<DoneMsg>(msg);
      REQUIRE(done.ok == true);
    }
  }

  REQUIRE(received_entries);
  REQUIRE(received_done);

  close(client_fd);
  if (worker_thread.joinable()) {
    worker_thread.join();
  }
}

TEST_CASE("Worker Frame Mode rejects non-image base64 payload", "[worker][ipc][frame_mode]") {
  int fds[2];
  REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

  int client_fd = fds[0];
  int worker_fd = fds[1];

  EngineFactory factory("mock");

  std::thread worker_thread([worker_fd, factory]() {
    WorkerConnection conn(worker_fd, factory);
    conn.run();
  });

  StartJobMsg start{
      .video_id = "v_bad_jpeg",
      .fps = 1.0,
      .engine = "mock",
      .confidence_threshold = 0.5,
  };
  REQUIRE(write_framed_message(client_fd, serialize_message(start)) == FramingStatus::Success);
  auto res_ready = read_framed_message(client_fd);
  REQUIRE(res_ready.status == FramingStatus::Success);

  // Valid base64 of non-image bytes ("hello") — must fail closed, not solid-fill.
  FrameMsg frame_msg{
      .video_id = "v_bad_jpeg",
      .ts_ms = 0,
      .jpeg_bytes = "aGVsbG8=",
  };
  REQUIRE(write_framed_message(client_fd, serialize_message(frame_msg)) == FramingStatus::Success);

  auto res_err = read_framed_message(client_fd);
  REQUIRE(res_err.status == FramingStatus::Success);
  Message err_msg = parse_message(res_err.payload);
  REQUIRE(std::holds_alternative<ErrorMsg>(err_msg));
  REQUIRE(std::get<ErrorMsg>(err_msg).message.find("JPEG 解码失败") != std::string::npos);

  close(client_fd);
  if (worker_thread.joinable()) {
    worker_thread.join();
  }
}

TEST_CASE("Worker Frame Mode error cases and invalid base64", "[worker][ipc][frame_mode]") {
  int fds[2];
  REQUIRE(socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

  int client_fd = fds[0];
  int worker_fd = fds[1];

  EngineFactory factory("mock");

  std::thread worker_thread([worker_fd, factory]() {
    WorkerConnection conn(worker_fd, factory);
    conn.run();
  });

  SECTION("frame before start_job") {
    FrameMsg frame_msg{.video_id = "v_err", .ts_ms = 0, .jpeg_bytes = kMinimalJpegB64};
    REQUIRE(write_framed_message(client_fd, serialize_message(frame_msg)) == FramingStatus::Success);

    auto res = read_framed_message(client_fd);
    REQUIRE(res.status == FramingStatus::Success);
    Message msg = parse_message(res.payload);
    REQUIRE(std::holds_alternative<ErrorMsg>(msg));
  }

  SECTION("invalid base64 characters") {
    StartJobMsg start{
        .video_id = "v_b64_err",
        .fps = 1.0,
        .engine = "mock",
        .confidence_threshold = 0.5,
    };

    REQUIRE(write_framed_message(client_fd, serialize_message(start)) == FramingStatus::Success);
    auto res_ready = read_framed_message(client_fd);
    REQUIRE(res_ready.status == FramingStatus::Success);

    FrameMsg bad_frame_msg{
        .video_id = "v_b64_err",
        .ts_ms = 0,
        .jpeg_bytes = "!!!invalid_b64!!!",
    };

    REQUIRE(write_framed_message(client_fd, serialize_message(bad_frame_msg)) ==
            FramingStatus::Success);
    auto res_err = read_framed_message(client_fd);
    REQUIRE(res_err.status == FramingStatus::Success);
    Message err_msg = parse_message(res_err.payload);
    REQUIRE(std::holds_alternative<ErrorMsg>(err_msg));
    REQUIRE(std::get<ErrorMsg>(err_msg).message.find("base64 解码失败") != std::string::npos);
  }

  close(client_fd);
  if (worker_thread.joinable()) {
    worker_thread.join();
  }
}
