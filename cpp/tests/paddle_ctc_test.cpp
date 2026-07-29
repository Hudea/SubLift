#include <catch2/catch_test_macros.hpp>
#include <string>
#include <vector>

#include "ppocr_ctc.hpp"

TEST_CASE("Paddle CTC - Greedy Decode", "[paddle][ctc]") {
  std::vector<std::string> dict = {"A", "B", "C", "D"};

  SECTION("Empty or invalid inputs") {
    auto res = sublift::paddle_detail::ctc_greedy_decode(nullptr, 0, 5, dict);
    REQUIRE(res.text.empty());
    REQUIRE(res.confidence == 0.0);
  }

  SECTION("All blank sequence") {
    std::vector<float> logits = {
        0.9f, 0.1f, 0.0f, 0.0f, 0.0f,
        0.8f, 0.2f, 0.0f, 0.0f, 0.0f,
    };
    auto res = sublift::paddle_detail::ctc_greedy_decode(logits.data(), 2, 5, dict);
    REQUIRE(res.text.empty());
    REQUIRE(res.confidence == 0.0);
  }

  SECTION("Simple sequence decoding without duplicates") {
    // 3 timesteps, 5 classes (0=blank, 1=A, 2=B, 3=C, 4=D)
    std::vector<float> logits = {
        0.1f, 0.9f, 0.0f, 0.0f, 0.0f,  // Step 0 -> 'A'
        0.1f, 0.0f, 0.8f, 0.0f, 0.1f,  // Step 1 -> 'B'
        0.95f, 0.0f, 0.0f, 0.05f, 0.0f  // Step 2 -> blank
    };

    auto res = sublift::paddle_detail::ctc_greedy_decode(logits.data(), 3, 5, dict);
    REQUIRE(res.text == "AB");
    REQUIRE(res.confidence > 0.84);
  }

  SECTION("Duplicate consecutive characters collapsed") {
    std::vector<float> logits = {
        0.1f, 0.9f, 0.0f, 0.0f, 0.0f,  // Step 0 -> 'A'
        0.1f, 0.9f, 0.0f, 0.0f, 0.0f,  // Step 1 -> 'A' (duplicate, collapsed)
        0.1f, 0.0f, 0.7f, 0.1f, 0.1f   // Step 2 -> 'B'
    };

    auto res = sublift::paddle_detail::ctc_greedy_decode(logits.data(), 3, 5, dict);
    REQUIRE(res.text == "AB");
  }

  SECTION("Blank separated duplicate characters preserved") {
    // Timestep 0: A (0.9), Timestep 1: blank (0.95), Timestep 2: A (0.85) -> "AA"
    std::vector<float> logits = {
        0.1f, 0.9f, 0.0f, 0.0f, 0.0f,   // Step 0 -> 'A'
        0.95f, 0.05f, 0.0f, 0.0f, 0.0f, // Step 1 -> blank
        0.1f, 0.85f, 0.0f, 0.0f, 0.0f   // Step 2 -> 'A'
    };

    auto res = sublift::paddle_detail::ctc_greedy_decode(logits.data(), 3, 5, dict);
    REQUIRE(res.text == "AA");
  }
}
