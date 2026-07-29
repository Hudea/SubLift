#include <catch2/catch_test_macros.hpp>
#include <stdexcept>

#include "sublift/paddle.hpp"

TEST_CASE("Paddle Stub - Availability and Constructor Behavior", "[paddle][stub]") {
#if defined(SUBLIFT_HAS_PADDLE) && SUBLIFT_HAS_PADDLE
  SECTION("Paddle is enabled in build") {
    REQUIRE(sublift::is_paddle_available());
    REQUIRE_NOTHROW(sublift::PaddleOcrEngine());
  }
#else
  SECTION("Paddle is disabled in default build") {
    REQUIRE_FALSE(sublift::is_paddle_available());
    REQUIRE_THROWS_AS(sublift::PaddleOcrEngine(), std::runtime_error);

    try {
      sublift::PaddleOcrEngine engine;
      (void)engine;
    } catch (const std::runtime_error& err) {
      std::string msg = err.what();
      REQUIRE(msg.find("PaddleOCR 不可用") != std::string::npos);
    }
  }
#endif
}
