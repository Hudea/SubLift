#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "sublift/cli/args.hpp"
#include "sublift/cli/runtime_guard.hpp"
#include "sublift/cli/srt.hpp"
#include "sublift/cli/worker_locator.hpp"
#include "sublift/models.hpp"

namespace fs = std::filesystem;
using sublift::cli::ParseStatus;

namespace {

struct EnvRestore {
  std::map<std::string, std::optional<std::string>> saved;

  void set(const char* key, const char* value) {
    const char* old = std::getenv(key);
    if (saved.find(key) == saved.end()) {
      saved[key] = old == nullptr ? std::nullopt : std::optional<std::string>{old};
    }
    if (value == nullptr) {
      ::unsetenv(key);
    } else {
      ::setenv(key, value, 1);
    }
  }

  ~EnvRestore() {
    for (const auto& [key, value] : saved) {
      if (value.has_value()) {
        ::setenv(key.c_str(), value->c_str(), 1);
      } else {
        ::unsetenv(key.c_str());
      }
    }
  }
};

[[nodiscard]] int run_logged(const std::string& cmd, const fs::path& stdout_path,
                             const fs::path& stderr_path) {
  const std::string wrapped = cmd + " >" + stdout_path.string() + " 2>" + stderr_path.string();
  const int rc = std::system(wrapped.c_str());
  if (rc == -1) return -1;
  if (WIFEXITED(rc)) return WEXITSTATUS(rc);
  return rc;
}

[[nodiscard]] std::string read_file(const fs::path& p) {
  std::ifstream in(p);
  return std::string{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

[[nodiscard]] fs::path make_test_video() {
  const fs::path video = fs::temp_directory_path() / "sublift_cli_test_video.mp4";
  if (fs::exists(video)) return video;
  const std::string cmd =
      "ffmpeg -y -f lavfi -i testsrc=size=320x240:rate=1 -t 1 -pix_fmt yuv420p " +
      video.string() + " >/dev/null 2>&1";
  std::system(cmd.c_str());
  return video;
}

#ifdef SUBLIFT_CLI_PATH
constexpr const char* kCliPath = SUBLIFT_CLI_PATH;
#else
constexpr const char* kCliPath = "";
#endif
#ifdef SUBLIFT_WORKER_BIN
constexpr const char* kWorkerBin = SUBLIFT_WORKER_BIN;
#else
constexpr const char* kWorkerBin = "";
#endif

}  // namespace

TEST_CASE("CLI parse extract arguments", "[cli][args]") {
  SECTION("defaults") {
    const auto r = sublift::cli::parse_extract_args({"sublift", "extract", "clip.mp4"});
    REQUIRE(r.status == ParseStatus::Ok);
    REQUIRE(r.args.video == "clip.mp4");
    REQUIRE(r.args.output == "output.srt");
    REQUIRE(r.args.fps == 5.0);
    REQUIRE(r.args.confidence == 0.5);
    REQUIRE(r.args.engine == "vision");
    REQUIRE(r.args.script == "auto");
    REQUIRE_FALSE(r.args.worker.has_value());
  }

  SECTION("custom flags") {
    const auto r = sublift::cli::parse_extract_args(
        {"sublift", "extract", "v.mkv", "-o", "out.srt", "--fps", "2.5", "--engine", "mock",
         "--script", "cjk", "--worker", "/tmp/sublift_worker"});
    REQUIRE(r.status == ParseStatus::Ok);
    REQUIRE(r.args.output == "out.srt");
    REQUIRE(r.args.fps == 2.5);
    REQUIRE(r.args.engine == "mock");
    REQUIRE(r.args.script == "cjk");
    REQUIRE(r.args.worker == fs::path{"/tmp/sublift_worker"});
  }

  SECTION("--runtime is a product error") {
    const auto r = sublift::cli::parse_extract_args(
        {"sublift", "extract", "v.mp4", "--runtime", "python"});
    REQUIRE(r.status == ParseStatus::Error);
    REQUIRE(r.error.find("--runtime") != std::string::npos);
    REQUIRE(r.error.find("not a product option") != std::string::npos);
  }

  SECTION("unknown engine") {
    const auto r =
        sublift::cli::parse_extract_args({"sublift", "extract", "v.mp4", "--engine", "onnx"});
    REQUIRE(r.status == ParseStatus::Error);
    REQUIRE(r.error.find("mock|vision|paddle") != std::string::npos);
  }

  SECTION("missing video") {
    const auto r = sublift::cli::parse_extract_args({"sublift", "extract"});
    REQUIRE(r.status == ParseStatus::Error);
    REQUIRE(r.error.find("<video>") != std::string::npos);
  }

  SECTION("help") {
    const auto r =
        sublift::cli::parse_extract_args({"sublift", "extract", "--help"});
    REQUIRE(r.status == ParseStatus::Help);
  }
}

TEST_CASE("CLI runtime env guard", "[cli][runtime]") {
  REQUIRE_FALSE(sublift::cli::product_runtime_env_error("").has_value());
  REQUIRE_FALSE(sublift::cli::product_runtime_env_error("   ").has_value());
  REQUIRE_FALSE(sublift::cli::product_runtime_env_error("cpp").has_value());
  REQUIRE_FALSE(sublift::cli::product_runtime_env_error("CPP").has_value());

  const auto python = sublift::cli::product_runtime_env_error("python");
  REQUIRE(python.has_value());
  REQUIRE(python->find("SUBLIFT_RUNTIME=python") != std::string::npos);
  REQUIRE(python->find("not a product option") != std::string::npos);

  const auto other = sublift::cli::product_runtime_env_error("oracle");
  REQUIRE(other.has_value());
  REQUIRE(other->find("oracle") != std::string::npos);
}

TEST_CASE("CLI SRT formatter", "[cli][srt]") {
  REQUIRE(sublift::cli::format_srt_timestamp(0) == "00:00:00,000");
  REQUIRE(sublift::cli::format_srt_timestamp(3661012) == "01:01:01,012");
  REQUIRE(sublift::cli::is_blank_text(""));
  REQUIRE(sublift::cli::is_blank_text(" \t\n"));
  REQUIRE_FALSE(sublift::cli::is_blank_text("hi"));

  const std::vector<sublift::SubtitleEntry> entries{
      {.start_ms = 0, .end_ms = 1000, .text = "hello"},
      {.start_ms = 1000, .end_ms = 2000, .text = "   "},
      {.start_ms = 2000, .end_ms = 3000, .text = "world"},
  };
  const auto [body, count] = sublift::cli::format_srt(entries);
  REQUIRE(count == 2);
  REQUIRE(body.find("1\n00:00:00,000 --> 00:00:01,000\nhello\n\n") != std::string::npos);
  REQUIRE(body.find("2\n00:00:02,000 --> 00:00:03,000\nworld\n\n") != std::string::npos);
  REQUIRE(body.find("   ") == std::string::npos);
}

TEST_CASE("CLI worker locator fail-closed", "[cli][worker]") {
  const fs::path tmp = fs::temp_directory_path() / "sublift_cli_locator";
  fs::create_directories(tmp);
  const fs::path real = tmp / "sublift_worker";
  {
    std::ofstream out(real);
    out << "#!/bin/sh\n";
  }

  SECTION("override wins when present") {
    sublift::cli::WorkerLocateContext ctx;
    ctx.override_path = real;
    ctx.env_path = tmp / "other";
    const auto found = sublift::cli::resolve_worker(ctx);
    REQUIRE(found.has_value());
    REQUIRE(*found == real);
  }

  SECTION("missing override does not fall through") {
    sublift::cli::WorkerLocateContext ctx;
    ctx.override_path = tmp / "missing_worker";
    ctx.env_path = real;
    ctx.cwd = tmp;
    REQUIRE_FALSE(sublift::cli::resolve_worker(ctx).has_value());
  }

  SECTION("missing env does not fall through") {
    sublift::cli::WorkerLocateContext ctx;
    ctx.env_path = tmp / "missing_env_worker";
    ctx.cwd = tmp;
    REQUIRE_FALSE(sublift::cli::resolve_worker(ctx).has_value());
  }

  SECTION("cwd candidate") {
    const fs::path build_bin = tmp / "build" / "cpp" / "bin";
    fs::create_directories(build_bin);
    const fs::path worker = build_bin / "sublift_worker";
    {
      std::ofstream out(worker);
      out << "x";
    }
    sublift::cli::WorkerLocateContext ctx;
    ctx.cwd = tmp;
    const auto found = sublift::cli::resolve_worker(ctx);
    REQUIRE(found.has_value());
    REQUIRE(found->filename() == "sublift_worker");
  }
}

TEST_CASE("CLI binary rejects python runtime and missing worker", "[cli][product]") {
  if (std::string{kCliPath}.empty() || !fs::exists(kCliPath)) {
    SKIP("sublift_cli binary not available");
  }

  EnvRestore env;
  env.set("SUBLIFT_RUNTIME", nullptr);

  const fs::path out_dir = fs::temp_directory_path() / "sublift_cli_product";
  fs::create_directories(out_dir);
  const fs::path stdout_path = out_dir / "stdout.txt";
  const fs::path stderr_path = out_dir / "stderr.txt";
  const fs::path srt = out_dir / "out.srt";

  SECTION("--runtime python exits 2") {
    const std::string cmd =
        std::string{"\""} + kCliPath + "\" extract clip.mp4 --runtime python";
    const int rc = run_logged(cmd, stdout_path, stderr_path);
    REQUIRE(rc == 2);
    REQUIRE(read_file(stderr_path).find("--runtime") != std::string::npos);
  }

  SECTION("SUBLIFT_RUNTIME=python exits 2") {
    env.set("SUBLIFT_RUNTIME", "python");
    const std::string cmd = std::string{"\""} + kCliPath + "\" extract clip.mp4 --engine mock";
    const int rc = run_logged(cmd, stdout_path, stderr_path);
    REQUIRE(rc == 2);
    REQUIRE(read_file(stderr_path).find("SUBLIFT_RUNTIME=python") != std::string::npos);
  }

  SECTION("resources status prints native contract") {
    const std::string cmd = std::string{"\""} + kCliPath + "\" resources status";
    const int rc = run_logged(cmd, stdout_path, stderr_path);
    (void)rc;
    const auto out = read_file(stdout_path) + read_file(stderr_path);
    REQUIRE(out.find("ppocrv6-small") != std::string::npos);
  }

  SECTION("missing --worker fails closed") {
    env.set("SUBLIFT_RUNTIME", nullptr);
    const fs::path video = out_dir / "clip.mp4";
    {
      std::ofstream out(video);
      out << "not-a-real-video";
    }
    const std::string cmd = std::string{"\""} + kCliPath + "\" extract \"" + video.string() +
                            "\" --engine mock --worker \"" + (out_dir / "no-such-worker").string() +
                            "\"";
    const int rc = run_logged(cmd, stdout_path, stderr_path);
    REQUIRE(rc == 1);
    REQUIRE(read_file(stderr_path).find("sublift_worker not found") != std::string::npos);
  }
}

TEST_CASE("CLI extract mock writes SRT over worker protocol", "[cli][product][srt]") {
  if (std::string{kCliPath}.empty() || !fs::exists(kCliPath) ||
      std::string{kWorkerBin}.empty() || !fs::exists(kWorkerBin)) {
    SKIP("sublift_cli or sublift_worker binary not available");
  }

  const fs::path video = make_test_video();
  if (!fs::exists(video)) {
    SKIP("ffmpeg could not create a synthetic test video");
  }

  EnvRestore env;
  env.set("SUBLIFT_RUNTIME", nullptr);
  env.set("SUBLIFT_WORKER_PATH", kWorkerBin);

  const fs::path out_dir = fs::temp_directory_path() / "sublift_cli_extract";
  fs::create_directories(out_dir);
  const fs::path srt = out_dir / "out.srt";
  fs::remove(srt);
  const fs::path stdout_path = out_dir / "stdout.txt";
  const fs::path stderr_path = out_dir / "stderr.txt";

  const std::string cmd = std::string{"\""} + kCliPath + "\" extract \"" + video.string() +
                          "\" --engine mock -o \"" + srt.string() + "\" --worker \"" +
                          kWorkerBin + "\"";
  const int rc = run_logged(cmd, stdout_path, stderr_path);
  REQUIRE(rc == 0);
  REQUIRE(fs::exists(srt));
  const std::string body = read_file(srt);
  // Mock OCR on testsrc may emit zero or more cues; the product contract is a
  // well-formed SRT file (empty is valid) written after a successful handshake.
  if (!body.empty()) {
    REQUIRE(body.find("-->") != std::string::npos);
  }
  const std::string out = read_file(stdout_path);
  REQUIRE(out.find("native C++ Worker") != std::string::npos);
  REQUIRE(out.find("完成：") != std::string::npos);
}
