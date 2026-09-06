#include <catch2/catch_test_macros.hpp>

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

#include "sublift/adapters/paddle.hpp"
#include "sublift/core/ocr_execution.hpp"

using namespace sublift;

namespace {

std::optional<std::string_view> sv(const char* s) {
  return s == nullptr ? std::nullopt : std::optional<std::string_view>{std::string_view{s}};
}

}  // namespace

TEST_CASE("Paddle provider names and parsing", "[ocr_execution]") {
  REQUIRE(paddle_provider_name(PaddleProvider::Cpu) == "cpu");
  REQUIRE(paddle_provider_name(PaddleProvider::Cuda) == "cuda");

  REQUIRE(parse_paddle_provider("cpu") == PaddleProvider::Cpu);
  REQUIRE(parse_paddle_provider("cuda") == PaddleProvider::Cuda);
  // Case-insensitive and tolerant of surrounding whitespace.
  REQUIRE(parse_paddle_provider("CPU") == PaddleProvider::Cpu);
  REQUIRE(parse_paddle_provider(" Cuda ") == PaddleProvider::Cuda);
  // Unknown providers are rejected (no auto mode).
  REQUIRE_FALSE(parse_paddle_provider("auto").has_value());
  REQUIRE_FALSE(parse_paddle_provider("tensorrt").has_value());
  REQUIRE_FALSE(parse_paddle_provider("").has_value());
}

TEST_CASE("resolve_paddle_execution defaults to CPU with no device", "[ocr_execution]") {
  const auto res = resolve_paddle_execution(std::nullopt, std::nullopt, std::nullopt, std::nullopt);
  REQUIRE(res.ok());
  REQUIRE(res.config.provider == PaddleProvider::Cpu);
  REQUIRE_FALSE(res.config.device_id.has_value());
}

TEST_CASE("resolve_paddle_execution honours explicit provider", "[ocr_execution]") {
  const auto cpu = resolve_paddle_execution(sv("cpu"), std::nullopt, std::nullopt, std::nullopt);
  REQUIRE(cpu.ok());
  REQUIRE(cpu.config.provider == PaddleProvider::Cpu);

  const auto cuda = resolve_paddle_execution(sv("cuda"), std::nullopt, std::nullopt, std::nullopt);
  REQUIRE(cuda.ok());
  REQUIRE(cuda.config.provider == PaddleProvider::Cuda);
  // CUDA without an explicit device defaults to device 0.
  REQUIRE(cuda.config.device_id == 0);
}

TEST_CASE("resolve_paddle_execution precedence explicit over environment", "[ocr_execution]") {
  // Explicit cuda beats env cpu.
  const auto a = resolve_paddle_execution(sv("cuda"), std::nullopt, sv("cpu"), std::nullopt);
  REQUIRE(a.ok());
  REQUIRE(a.config.provider == PaddleProvider::Cuda);

  // Env used only when explicit absent.
  const auto b = resolve_paddle_execution(std::nullopt, std::nullopt, sv("cuda"), std::nullopt);
  REQUIRE(b.ok());
  REQUIRE(b.config.provider == PaddleProvider::Cuda);
  // CUDA defaults to device 0 regardless of which source selected it.
  REQUIRE(b.config.device_id == 0);

  // Empty explicit values fall back to the environment.
  const auto e = resolve_paddle_execution(sv(""), std::nullopt, sv("cuda"), std::nullopt);
  REQUIRE(e.ok());
  REQUIRE(e.config.provider == PaddleProvider::Cuda);

  // An invalid explicit value wins over a valid environment value (fail-closed).
  const auto f = resolve_paddle_execution(sv("auto"), std::nullopt, sv("cpu"), std::nullopt);
  REQUIRE_FALSE(f.ok());

  // An invalid environment device is rejected when it takes effect.
  const auto g = resolve_paddle_execution(sv("cuda"), std::nullopt, std::nullopt, sv("abc"));
  REQUIRE_FALSE(g.ok());

  // Explicit device beats env device.
  const auto c = resolve_paddle_execution(sv("cuda"), sv("1"), std::nullopt, sv("3"));
  REQUIRE(c.ok());
  REQUIRE(c.config.device_id == 1);

  // Env device used only when explicit absent.
  const auto d = resolve_paddle_execution(sv("cuda"), std::nullopt, std::nullopt, sv("2"));
  REQUIRE(d.ok());
  REQUIRE(d.config.device_id == 2);
}

TEST_CASE("resolve_paddle_execution rejects invalid provider", "[ocr_execution]") {
  const auto res = resolve_paddle_execution(sv("auto"), std::nullopt, std::nullopt, std::nullopt);
  REQUIRE_FALSE(res.ok());
  REQUIRE(res.error.find("auto") != std::string::npos);

  const auto env_bad = resolve_paddle_execution(std::nullopt, std::nullopt, sv("gpu"), std::nullopt);
  REQUIRE_FALSE(env_bad.ok());
  REQUIRE(env_bad.error.find("gpu") != std::string::npos);
}

TEST_CASE("resolve_paddle_execution CPU rejects a device id", "[ocr_execution]") {
  const auto res = resolve_paddle_execution(sv("cpu"), sv("0"), std::nullopt, std::nullopt);
  REQUIRE_FALSE(res.ok());
  REQUIRE(res.error.find("CPU") != std::string::npos);

  // CPU with an env-supplied device is also rejected.
  const auto env = resolve_paddle_execution(std::nullopt, std::nullopt, sv("cpu"), sv("1"));
  REQUIRE_FALSE(env.ok());
}

TEST_CASE("resolve_paddle_execution rejects invalid device ids", "[ocr_execution]") {
  const auto negative = resolve_paddle_execution(sv("cuda"), sv("-1"), std::nullopt, std::nullopt);
  REQUIRE_FALSE(negative.ok());

  const auto non_numeric = resolve_paddle_execution(sv("cuda"), sv("abc"), std::nullopt, std::nullopt);
  REQUIRE_FALSE(non_numeric.ok());

  // int32 boundary is locked: INT32_MAX passes, INT32_MAX+1 and huge values fail.
  const auto max_ok = resolve_paddle_execution(sv("cuda"), sv("2147483647"), std::nullopt, std::nullopt);
  REQUIRE(max_ok.ok());
  REQUIRE(max_ok.config.device_id == 2147483647);
  REQUIRE_FALSE(
      resolve_paddle_execution(sv("cuda"), sv("2147483648"), std::nullopt, std::nullopt).ok());
  REQUIRE_FALSE(resolve_paddle_execution(sv("cuda"), sv("99999999999999999999"), std::nullopt,
                                         std::nullopt)
                    .ok());

  const auto empty_is_default = resolve_paddle_execution(sv("cuda"), sv(""), std::nullopt, std::nullopt);
  REQUIRE(empty_is_default.ok());
  REQUIRE(empty_is_default.config.device_id == 0);

  // Surrounding whitespace is tolerated; whitespace-only counts as unset.
  const auto spaced = resolve_paddle_execution(sv("cuda"), sv(" 2 "), std::nullopt, std::nullopt);
  REQUIRE(spaced.ok());
  REQUIRE(spaced.config.device_id == 2);
  const auto blank = resolve_paddle_execution(sv("cuda"), sv("   "), std::nullopt, std::nullopt);
  REQUIRE(blank.ok());
  REQUIRE(blank.config.device_id == 0);
  const auto blank_cpu = resolve_paddle_execution(sv("cpu"), sv("   "), std::nullopt, std::nullopt);
  REQUIRE(blank_cpu.ok());
  REQUIRE(blank_cpu.config.provider == PaddleProvider::Cpu);
}

TEST_CASE("resolve_paddle_execution inherits provider spelling tolerance", "[ocr_execution]") {
  const auto upper = resolve_paddle_execution(sv("CPU"), std::nullopt, std::nullopt, std::nullopt);
  REQUIRE(upper.ok());
  REQUIRE(upper.config.provider == PaddleProvider::Cpu);

  const auto spaced =
      resolve_paddle_execution(sv(" Cuda "), std::nullopt, std::nullopt, std::nullopt);
  REQUIRE(spaced.ok());
  REQUIRE(spaced.config.provider == PaddleProvider::Cuda);
  REQUIRE(spaced.config.device_id == 0);
}

TEST_CASE("resolve_paddle_execution treats empty strings as unset", "[ocr_execution]") {
  const auto res = resolve_paddle_execution(sv(""), sv(""), sv(""), sv(""));
  REQUIRE(res.ok());
  REQUIRE(res.config.provider == PaddleProvider::Cpu);
  REQUIRE_FALSE(res.config.device_id.has_value());
}

TEST_CASE("format_paddle_execution renders provider and device", "[ocr_execution]") {
  PaddleExecutionConfig cpu;
  REQUIRE(format_paddle_execution(cpu) == "cpu");

  PaddleExecutionConfig cuda;
  cuda.provider = PaddleProvider::Cuda;
  cuda.device_id = 0;
  REQUIRE(format_paddle_execution(cuda) == "cuda:0");

  cuda.device_id = 2;
  REQUIRE(format_paddle_execution(cuda) == "cuda:2");
}

TEST_CASE("validate_paddle_execution is fail-closed for cuda", "[ocr_execution][paddle]") {
  PaddleExecutionConfig cpu;
  REQUIRE(validate_paddle_execution(cpu).empty());

  PaddleExecutionConfig cuda;
  cuda.provider = PaddleProvider::Cuda;
  cuda.device_id = 0;
  const std::string err = validate_paddle_execution(cuda);
  REQUIRE_FALSE(err.empty());
  REQUIRE(err.find("cuda") != std::string::npos);
}

TEST_CASE("PaddleOptions defaults to CPU execution backend", "[ocr_execution][paddle]") {
  PaddleOptions opts;
  REQUIRE(opts.execution.provider == PaddleProvider::Cpu);
  REQUIRE_FALSE(opts.execution.device_id.has_value());
}

TEST_CASE("PaddleOcrEngine rejects cuda execution at construction", "[ocr_execution][paddle]") {
  PaddleOptions opts;
  opts.execution.provider = PaddleProvider::Cuda;
  opts.execution.device_id = 0;
  try {
    PaddleOcrEngine engine(opts);
    FAIL("expected cuda execution backend to be rejected");
  } catch (const std::exception& e) {
    REQUIRE(std::string(e.what()).find("cuda") != std::string::npos);
  }
}
