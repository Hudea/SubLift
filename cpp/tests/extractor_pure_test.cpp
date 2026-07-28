#include <catch2/catch_test_macros.hpp>
#include <limits>
#include <nlohmann/json.hpp>
#include <stdexcept>
#include <string>

#include "sublift/extractor.hpp"
#include "sublift/ffmpeg.hpp"

using namespace sublift;
using namespace sublift::ffmpeg;

TEST_CASE("validate_output_crop validation rules", "[extractor][pure]") {
  SECTION("Valid crops") {
    REQUIRE_NOTHROW(validate_output_crop(SourceBox{0, 0, 1920, 1080}, 1920, 1080));
    REQUIRE_NOTHROW(validate_output_crop(SourceBox{100, 200, 300, 400}, 1920, 1080));
    REQUIRE_NOTHROW(validate_output_crop(SourceBox{100, 100, 1820, 980}, 1920, 1080));
    REQUIRE_NOTHROW(validate_output_crop(SourceBox{0, 0, 319, 239}, 640, 480));
  }

  SECTION("Negative x/y coordinates") {
    try {
      validate_output_crop(SourceBox{-1, 0, 100, 100}, 1920, 1080);
      FAIL("Should have thrown std::invalid_argument");
    } catch (const std::invalid_argument& exc) {
      const std::string msg = exc.what();
      REQUIRE(msg.find("x/y 不能为负") != std::string::npos);
      REQUIRE(msg.find("requested=[-1, 0, 100, 100]") != std::string::npos);
      REQUIRE(msg.find("source=1920x1080") != std::string::npos);
    }

    try {
      validate_output_crop(SourceBox{0, -10, 100, 100}, 1920, 1080);
      FAIL("Should have thrown std::invalid_argument");
    } catch (const std::invalid_argument& exc) {
      const std::string msg = exc.what();
      REQUIRE(msg.find("x/y 不能为负") != std::string::npos);
    }
  }

  SECTION("Non-positive width/height") {
    try {
      validate_output_crop(SourceBox{0, 0, 0, 100}, 1920, 1080);
      FAIL("Should have thrown std::invalid_argument");
    } catch (const std::invalid_argument& exc) {
      const std::string msg = exc.what();
      REQUIRE(msg.find("width/height 必须为正") != std::string::npos);
    }

    try {
      validate_output_crop(SourceBox{0, 0, 100, -5}, 1920, 1080);
      FAIL("Should have thrown std::invalid_argument");
    } catch (const std::invalid_argument& exc) {
      const std::string msg = exc.what();
      REQUIRE(msg.find("width/height 必须为正") != std::string::npos);
    }
  }

  SECTION("Out of bounds crop") {
    try {
      validate_output_crop(SourceBox{1000, 0, 1000, 1080}, 1920, 1080);
      FAIL("Should have thrown std::invalid_argument");
    } catch (const std::invalid_argument& exc) {
      const std::string msg = exc.what();
      REQUIRE(msg.find("越界") != std::string::npos);
    }

    try {
      validate_output_crop(SourceBox{0, 500, 1920, 600}, 1920, 1080);
      FAIL("Should have thrown std::invalid_argument");
    } catch (const std::invalid_argument& exc) {
      const std::string msg = exc.what();
      REQUIRE(msg.find("越界") != std::string::npos);
    }
  }

  SECTION("Large int overflow protection") {
    // Large int32 x + w must overflow if 32-bit addition without 64-bit promotion
    try {
      validate_output_crop(SourceBox{2'000'000'000, 0, 1'000'000'000, 1080}, 1920, 1080);
      FAIL("Should have thrown std::invalid_argument due to out of bounds");
    } catch (const std::invalid_argument& exc) {
      const std::string msg = exc.what();
      REQUIRE(msg.find("越界") != std::string::npos);
    }
  }
}

TEST_CASE("build_output_vf string formatting", "[extractor][pure]") {
  SECTION("Full frame") {
    REQUIRE(build_output_vf(1.0, nullptr) == "fps=1.0");
    REQUIRE(build_output_vf(2.5, nullptr) == "fps=2.5");
    std::optional<SourceBox> no_crop = std::nullopt;
    REQUIRE(build_output_vf(1.0, no_crop) == "fps=1.0");
  }

  SECTION("ROI crop") {
    SourceBox crop{100, 50, 300, 200};
    REQUIRE(build_output_vf(1.0, &crop) ==
            "fps=1.0,format=rgb24,crop=300:200:100:50:exact=1");

    std::optional<SourceBox> opt_crop = SourceBox{0, 0, 1920, 1080};
    REQUIRE(build_output_vf(0.5, opt_crop) ==
            "fps=0.5,format=rgb24,crop=1920:1080:0:0:exact=1");
  }
}

TEST_CASE("assess_display_transform and is_identity_display_matrix", "[extractor][pure]") {
  SECTION("No tags or side_data") {
    nlohmann::json stream = nlohmann::json::object({{"width", 1920}, {"height", 1080}});
    auto [ok, note] = assess_display_transform(stream);
    REQUIRE(ok);
    REQUIRE_FALSE(note.has_value());
  }

  SECTION("tags.rotate == '0'") {
    nlohmann::json stream = nlohmann::json::object({
        {"width", 1920},
        {"height", 1080},
        {"tags", {{"rotate", "0"}}}
    });
    auto [ok, note] = assess_display_transform(stream);
    REQUIRE(ok);
    REQUIRE_FALSE(note.has_value());
  }

  SECTION("tags.rotate == 90 (integer)") {
    nlohmann::json stream = nlohmann::json::object({
        {"width", 1920},
        {"height", 1080},
        {"tags", {{"rotate", 90}}}
    });
    auto [ok, note] = assess_display_transform(stream);
    REQUIRE_FALSE(ok);
    REQUIRE(note.has_value());
    REQUIRE(note.value() == "stream.tags.rotate=90");
  }

  SECTION("tags.rotate == '90'") {
    nlohmann::json stream = nlohmann::json::object({
        {"width", 1920},
        {"height", 1080},
        {"tags", {{"rotate", "90"}}}
    });
    auto [ok, note] = assess_display_transform(stream);
    REQUIRE_FALSE(ok);
    REQUIRE(note.has_value());
    REQUIRE(note.value() == "stream.tags.rotate='90'");
  }

  SECTION("side_data rotation == 0.0") {
    nlohmann::json stream = nlohmann::json::object({
        {"side_data_list", nlohmann::json::array({
            {{"side_data_type", "Display Matrix"}, {"rotation", 0.0}}
        })}
    });
    auto [ok, note] = assess_display_transform(stream);
    REQUIRE(ok);
    REQUIRE_FALSE(note.has_value());
  }

  SECTION("side_data rotation == -90.0") {
    nlohmann::json stream = nlohmann::json::object({
        {"side_data_list", nlohmann::json::array({
            {{"side_data_type", "Display Matrix"}, {"rotation", -90.0}}
        })}
    });
    auto [ok, note] = assess_display_transform(stream);
    REQUIRE_FALSE(ok);
    REQUIRE(note.has_value());
    REQUIRE(note.value().find("non-identity display matrix") != std::string::npos);
  }

  SECTION("16.16 fixed-point identity matrix") {
    nlohmann::json stream = nlohmann::json::object({
        {"side_data_list", nlohmann::json::array({
            {{"side_data_type", "Display Matrix"},
             {"displaymatrix", "65536 0 0 0 65536 0 0 0 1073741824"}}
        })}
    });
    auto [ok, note] = assess_display_transform(stream);
    REQUIRE(ok);
    REQUIRE_FALSE(note.has_value());
  }

  SECTION("displaymatrix is null but matrix is identity array fallback") {
    nlohmann::json stream = nlohmann::json::object({
        {"side_data_list", nlohmann::json::array({
            {{"side_data_type", "Display Matrix"},
             {"displaymatrix", nullptr},
             {"matrix", nlohmann::json::array({1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0})}}
        })}
    });
    auto [ok, note] = assess_display_transform(stream);
    REQUIRE(ok);
    REQUIRE_FALSE(note.has_value());
  }

  SECTION("3x3 float identity matrix array") {
    nlohmann::json stream = nlohmann::json::object({
        {"side_data_list", nlohmann::json::array({
            {{"side_data_type", "Display Matrix"},
             {"matrix", nlohmann::json::array({1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0})}}
        })}
    });
    auto [ok, note] = assess_display_transform(stream);
    REQUIRE(ok);
    REQUIRE_FALSE(note.has_value());
  }

  SECTION("Non-identity matrix string") {
    nlohmann::json stream = nlohmann::json::object({
        {"side_data_list", nlohmann::json::array({
            {{"side_data_type", "Display Matrix"},
             {"displaymatrix", "0 -65536 0 65536 0 0 0 0 1073741824"}}
        })}
    });
    auto [ok, note] = assess_display_transform(stream);
    REQUIRE_FALSE(ok);
    REQUIRE(note.has_value());
    REQUIRE(note.value().find("non-identity display matrix") != std::string::npos);
  }

  SECTION("Invalid side_data_list item") {
    nlohmann::json stream = nlohmann::json::object({
        {"side_data_list", nlohmann::json::array({"invalid_string"})}
    });
    auto [ok, note] = assess_display_transform(stream);
    REQUIRE_FALSE(ok);
    REQUIRE(note.has_value());
    REQUIRE(note.value() == "stream.side_data_list 项不可解析");
  }
}

class DummyExtractor : public IExtractor {
public:
  void cancel() override { cancelled_ = true; }
  [[nodiscard]] std::optional<SourceBox> output_crop() const override { return crop_; }

  bool cancelled_{false};
  std::optional<SourceBox> crop_{SourceBox{10, 10, 100, 100}};
};

TEST_CASE("IExtractor polymorphic usage", "[extractor][pure]") {
  DummyExtractor dummy;
  IExtractor& extractor = dummy;

  REQUIRE(extractor.output_crop().has_value());
  REQUIRE(extractor.output_crop()->x == 10);

  extractor.cancel();
  REQUIRE(dummy.cancelled_);
}
