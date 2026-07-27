#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

#include "sublift/detector.hpp"
#include "sublift/fixed_detector.hpp"
#include "sublift/image.hpp"
#include "sublift/mock_ocr.hpp"
#include "sublift/models.hpp"
#include "sublift/ocr.hpp"

namespace {

// Small dummy view; MockOcrEngine ignores pixels but recognize() needs an arg.
sublift::ImageView dummy_view() {
  static sublift::ImageBuffer buf(2, 2, sublift::PixelFormat::RGB24);
  return buf.view();
}

sublift::OcrResult res(const char* text, double conf) {
  return sublift::OcrResult{text, conf, {}};
}

}  // namespace

TEST_CASE("MockOcrEngine fixed text/confidence", "[ocr]") {
  sublift::MockOcrEngine mock("你好", 0.8);
  REQUIRE(mock.call_count() == 0);
  const auto r1 = mock.recognize(dummy_view());
  const auto r2 = mock.recognize(dummy_view());
  REQUIRE(r1.text == "你好");
  REQUIRE(r1.confidence == 0.8);
  REQUIRE(r1.lines.empty());
  // Fixed mode repeats indefinitely and still advances call_count.
  REQUIRE(r2.text == "你好");
  REQUIRE(mock.call_count() == 2);
}

TEST_CASE("MockOcrEngine from lines (join + mean conf)", "[ocr]") {
  std::vector<sublift::OcrLine> lines{
      sublift::OcrLine{"上行", 0.6, {}},
      sublift::OcrLine{"下行", 1.0, {}},
  };
  sublift::MockOcrEngine mock(std::span<const sublift::OcrLine>{lines});
  const auto r = mock.recognize(dummy_view());
  REQUIRE(r.text == "上行\n下行");
  REQUIRE(std::abs(r.confidence - 0.8) < 1e-9);
  REQUIRE(r.lines.size() == 2);
}

TEST_CASE("MockOcrEngine sequence order + exhaustion throws", "[ocr]") {
  sublift::MockOcrEngine mock(
      std::vector<sublift::OcrResult>{res("A", 0.9), res("B", 0.7)});
  REQUIRE(mock.call_count() == 0);
  REQUIRE(mock.recognize(dummy_view()).text == "A");
  REQUIRE(mock.recognize(dummy_view()).text == "B");
  REQUIRE(mock.call_count() == 2);
  REQUIRE_THROWS_AS(mock.recognize(dummy_view()), std::out_of_range);
}

TEST_CASE("MockOcrEngine injectable via IOcrEngine&", "[ocr]") {
  sublift::MockOcrEngine mock("x", 1.0);
  sublift::IOcrEngine& engine = mock;
  REQUIRE(engine.recognize(dummy_view()).text == "x");
}

TEST_CASE("FixedRegionDetector returns constructed box", "[detector]") {
  const sublift::FrameLocalBox box{10, 20, 100, 40};
  sublift::FixedRegionDetector det(box);

  // Different frame sizes must not change the returned region.
  sublift::Frame small{0, sublift::ImageBuffer(4, 4, sublift::PixelFormat::RGB24)};
  sublift::Frame large{33, sublift::ImageBuffer(640, 480, sublift::PixelFormat::RGB24)};

  const auto r1 = det.detect(small);
  const auto r2 = det.detect(large);
  REQUIRE(r1.has_value());
  REQUIRE(r2.has_value());
  REQUIRE(r1->box == box);
  REQUIRE(r2->box == box);
}

TEST_CASE("FixedRegionDetector injectable via IDetector&", "[detector]") {
  sublift::FixedRegionDetector det(sublift::FrameLocalBox{0, 0, 8, 8});
  sublift::IDetector& detector = det;
  sublift::Frame f{0, sublift::ImageBuffer(8, 8, sublift::PixelFormat::RGB24)};
  const auto r = detector.detect(f);
  REQUIRE(r.has_value());
  REQUIRE(r->box == sublift::FrameLocalBox{0, 0, 8, 8});
}
