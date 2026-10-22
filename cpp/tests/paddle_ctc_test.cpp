#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstdint>
#include <limits>
#include <random>
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

  SECTION("Confidence follows RapidOCR five-decimal rounding") {
    std::vector<float> logits = {
        0.0f, 0.123456f, 0.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 0.654321f, 0.0f, 0.0f,
    };
    auto res =
        sublift::paddle_detail::ctc_greedy_decode(logits.data(), 2, 5, dict);
    REQUIRE(res.text == "AB");
    REQUIRE(res.confidence == 0.38889);
  }
}

namespace {

double reference_decode(const std::vector<float>& logits,
                        int seq_len,
                        int num_classes,
                        const std::vector<std::string>& dictionary,
                        std::string* text,
                        std::vector<std::int32_t>* tokens) {
  text->clear();
  tokens->clear();
  double score_sum = 0.0;
  int valid_count = 0;
  int last_index = 0;
  const auto round_to_five = [](double value) {
    return std::round(value * 100000.0) / 100000.0;
  };
  for (int i = 0; i < seq_len; ++i) {
    const float* row = logits.data() + static_cast<std::size_t>(i) * num_classes;
    const int max_idx = sublift::paddle_detail::detail::argmax_first_scalar(
        row, num_classes);
    const float max_val = row[max_idx];
    if (max_idx != 0 && max_idx != last_index) {
      const std::size_t dict_idx = static_cast<std::size_t>(max_idx - 1);
      if (dict_idx < dictionary.size()) {
        *text += dictionary[dict_idx];
        tokens->push_back(max_idx);
        score_sum += round_to_five(static_cast<double>(max_val));
        valid_count++;
      }
    }
    last_index = max_idx;
  }
  return valid_count > 0
             ? round_to_five(score_sum / static_cast<double>(valid_count))
             : 0.0;
}

}  // namespace

TEST_CASE("Paddle CTC vectorized argmax matches the scalar contract",
          "[paddle][ctc][argmax][equivalence]") {
  std::mt19937 rng(20261022U);
  std::uniform_real_distribution<float> noise(-1.0F, 1.0F);
  for (const int count : {1, 2, 3, 7, 8, 9, 16, 17, 33, 64, 257, 18710}) {
    std::vector<float> values(static_cast<std::size_t>(count));
    for (auto& value : values) {
      value = noise(rng);
    }
    INFO("random count=" << count);
    CHECK(sublift::paddle_detail::detail::argmax_first(
              values.data(), count) ==
          sublift::paddle_detail::detail::argmax_first_scalar(
              values.data(), count));

    std::fill(values.begin(), values.end(), 0.25F);
    INFO("all-equal count=" << count);
    CHECK(sublift::paddle_detail::detail::argmax_first(values.data(), count) == 0);

    std::fill(values.begin(), values.end(), -1.0F);
    values[static_cast<std::size_t>(count) / 2U] = 0.5F;
    INFO("single-max count=" << count);
    CHECK(sublift::paddle_detail::detail::argmax_first(values.data(), count) ==
          static_cast<int>(static_cast<std::size_t>(count) / 2U));

    std::fill(values.begin(), values.end(), -1.0F);
    values[0] = 2.0F;
    values[static_cast<std::size_t>(count) - 1U] = 2.0F;
    INFO("tie-first-wins count=" << count);
    CHECK(sublift::paddle_detail::detail::argmax_first(values.data(), count) == 0);
  }

  // NaN never wins and never replaces the running maximum; a leading NaN
  // leaves the index at 0 exactly like the scalar loop.
  std::vector<float> with_nan{std::nanf(""), 0.1F, 0.2F, 0.3F, -0.4F, 0.05F, 0.2F, 0.9F, 0.9F, 0.1F};
  CHECK(sublift::paddle_detail::detail::argmax_first(with_nan.data(),
                                                    static_cast<int>(with_nan.size())) ==
        sublift::paddle_detail::detail::argmax_first_scalar(
            with_nan.data(), static_cast<int>(with_nan.size())));
  CHECK(sublift::paddle_detail::detail::argmax_first(with_nan.data(),
                                                    static_cast<int>(with_nan.size())) == 0);
  with_nan[3] = std::nanf("");
  with_nan[7] = 1.0F;
  CHECK(sublift::paddle_detail::detail::argmax_first(with_nan.data(),
                                                    static_cast<int>(with_nan.size())) == 0);

  // NaN in the middle is skipped, not treated as a maximum.
  std::vector<float> mid_nan{0.1F, std::nanf(""), 0.9F, 0.9F, 0.2F, -1.0F, 0.4F};
  CHECK(sublift::paddle_detail::detail::argmax_first(mid_nan.data(),
                                                    static_cast<int>(mid_nan.size())) == 2);

  // +/-inf and ties.
  std::vector<float> with_inf{-1.0F, 1.0F, 3.0F, -1.0F, 3.0F, -1.0F, -1.0F, -1.0F, 2.0F};
  CHECK(sublift::paddle_detail::detail::argmax_first(with_inf.data(),
                                                    static_cast<int>(with_inf.size())) == 2);
  with_inf[5] = std::numeric_limits<float>::quiet_NaN();
  CHECK(sublift::paddle_detail::detail::argmax_first(with_inf.data(),
                                                    static_cast<int>(with_inf.size())) == 2);
}

TEST_CASE("Paddle CTC decode text and confidence match the scalar reference",
          "[paddle][ctc][equivalence]") {
  std::mt19937 rng(7U);
  std::uniform_real_distribution<float> noise(0.0F, 1.0F);
  const std::vector<std::string> dictionary{"A", "B", "C", "\xE4\xB8\xAD"};
  constexpr int kSeq = 24;
  constexpr int kClasses = 5;
  for (int iteration = 0; iteration < 50; ++iteration) {
    std::vector<float> logits(
        static_cast<std::size_t>(kSeq) * kClasses);
    for (auto& value : logits) {
      value = noise(rng);
    }
    // Sprinkle ties so first-occurrence semantics is exercised.
    logits[6U * kClasses + 1U] = 1.0F;
    logits[6U * kClasses + 2U] = 1.0F;

    const auto actual = sublift::paddle_detail::ctc_greedy_decode(
        logits.data(), kSeq, kClasses, dictionary);
    std::string expected_text;
    std::vector<std::int32_t> expected_tokens;
    const double expected_confidence = reference_decode(
        logits, kSeq, kClasses, dictionary, &expected_text, &expected_tokens);
    INFO("iteration=" << iteration);
    CHECK(actual.text == expected_text);
    CHECK(actual.token_indices == expected_tokens);
    CHECK(actual.confidence == expected_confidence);
  }
}
