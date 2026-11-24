#import <CoreGraphics/CoreGraphics.h>
#import <CoreText/CoreText.h>
#import <Foundation/Foundation.h>

#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <nlohmann/json.hpp>
#include <opencv2/imgcodecs.hpp>
#include <opencv2/imgproc.hpp>
#include <string>
#include <thread>
#include <vector>

#include <sublift/adapters/apple_vision/apple_vision_ocr.hpp>
#include <sublift/core/image_buffer.hpp>
#include <sublift/core/image_view.hpp>

using namespace sublift;
using namespace sublift::adapters::apple_vision;

#ifndef SUBLIFT_PROJECT_SOURCE_DIR
#define SUBLIFT_PROJECT_SOURCE_DIR ""
#endif

#ifndef SUBLIFT_PARITY_PADDLE_MANIFEST
#define SUBLIFT_PARITY_PADDLE_MANIFEST ""
#endif

#if defined(SUBLIFT_ENABLE_VISION) && SUBLIFT_ENABLE_VISION

namespace {

ImageBuffer draw_synthetic_text(
    int width, int height,
    const std::string& text,
    const std::string& font_name_utf8 = "Helvetica") {
  std::vector<std::uint8_t> rgba_data(
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U, 255);

  CGColorSpaceRef color_space = CGColorSpaceCreateDeviceRGB();
  const uint32_t bitmap_info =
      static_cast<uint32_t>(kCGImageAlphaPremultipliedLast) |
      static_cast<uint32_t>(kCGBitmapByteOrder32Big);
  CGContextRef ctx = CGBitmapContextCreate(
      rgba_data.data(), width, height, 8, width * 4, color_space, bitmap_info);

  if (ctx) {
    CGContextSetRGBFillColor(ctx, 1.0, 1.0, 1.0, 1.0);
    CGContextFillRect(ctx, CGRectMake(0, 0, width, height));
    CGContextSetRGBFillColor(ctx, 0.0, 0.0, 0.0, 1.0);

    CFStringRef requested_font = CFStringCreateWithCString(
        kCFAllocatorDefault, font_name_utf8.c_str(), kCFStringEncodingUTF8);
    CTFontRef font = nullptr;
    if (requested_font) {
      font = CTFontCreateWithName(requested_font, 36.0, nullptr);
      CFRelease(requested_font);
    }
    if (!font && font_name_utf8 != "Helvetica") {
      font = CTFontCreateWithName(CFSTR("PingFangSC-Regular"), 36.0, nullptr);
      if (!font) {
        font = CTFontCreateWithName(CFSTR("STHeitiSC-Light"), 36.0, nullptr);
      }
    }
    if (!font) {
      font = CTFontCreateWithName(CFSTR("Helvetica"), 36.0, nullptr);
    }

    CFStringRef str = CFStringCreateWithCString(
        kCFAllocatorDefault, text.c_str(), kCFStringEncodingUTF8);
    if (font && str) {
      CFStringRef keys[] = {kCTFontAttributeName, kCTForegroundColorFromContextAttributeName};
      CFTypeRef values[] = {font, kCFBooleanTrue};
      CFDictionaryRef attributes = CFDictionaryCreate(
          kCFAllocatorDefault,
          reinterpret_cast<const void**>(keys),
          reinterpret_cast<const void**>(values),
          2,
          &kCFTypeDictionaryKeyCallBacks,
          &kCFTypeDictionaryValueCallBacks);
      CFAttributedStringRef attr_str =
          CFAttributedStringCreate(kCFAllocatorDefault, str, attributes);
      CTLineRef line = CTLineCreateWithAttributedString(attr_str);

      CGContextSetTextPosition(ctx, 20, height / 2 - 10);
      CTLineDraw(line, ctx);

      CFRelease(line);
      CFRelease(attr_str);
      CFRelease(attributes);
    }
    if (font) CFRelease(font);
    if (str) CFRelease(str);
    CGContextRelease(ctx);
  }
  CGColorSpaceRelease(color_space);

  ImageBuffer buf(width, height, PixelFormat::RGB24);
  std::uint8_t* dst = buf.data();
  for (int i = 0; i < width * height; ++i) {
    dst[i * 3 + 0] = rgba_data[i * 4 + 0];
    dst[i * 3 + 1] = rgba_data[i * 4 + 1];
    dst[i * 3 + 2] = rgba_data[i * 4 + 2];
  }
  return buf;
}

std::filesystem::path resolve_manifest_path() {
  if (std::string(SUBLIFT_PARITY_PADDLE_MANIFEST).size() > 0) {
    std::filesystem::path p{SUBLIFT_PARITY_PADDLE_MANIFEST};
    if (std::filesystem::exists(p)) {
      return p;
    }
  }
  if (std::string(SUBLIFT_PROJECT_SOURCE_DIR).size() > 0) {
    std::filesystem::path p =
        std::filesystem::path{SUBLIFT_PROJECT_SOURCE_DIR} /
        "benchmark/parity/fixtures/paddle/manifest.json";
    if (std::filesystem::exists(p)) {
      return p;
    }
  }
  return {};
}

}  // namespace

TEST_CASE("AppleVisionOcrEngine synthetic CoreText & pixel-path contract", "[apple_vision][adapter]") {
  if (!is_vision_available()) {
    SKIP("Apple Vision framework unavailable at runtime");
  }

  AppleVisionOcrEngine engine;
  REQUIRE(engine.engine_name() == "apple_vision");

  SECTION("empty and blank buffers return empty result") {
    ImageView empty_view;
    auto empty_res = engine.recognize(empty_view);
    REQUIRE(empty_res.lines.empty());
    REQUIRE(empty_res.text.empty());
    REQUIRE(empty_res.confidence == 0.0);

    ImageBuffer blank(200, 100, PixelFormat::RGB24);
    auto blank_res = engine.recognize(blank.view());
    REQUIRE(blank_res.lines.empty());
  }

  SECTION("synthetic English recognition (RGB24)") {
    ImageBuffer img = draw_synthetic_text(400, 100, "SUBLIFT", "Helvetica");
    auto res = engine.recognize(img.view());
    REQUIRE_FALSE(res.lines.empty());
    REQUIRE(res.confidence > 0.0);
    REQUIRE_FALSE(res.text.empty());
    REQUIRE(res.lines[0].box.width > 0);
    REQUIRE(res.lines[0].box.height > 0);
  }

  SECTION("synthetic CJK + Latin recognition") {
    ImageBuffer img = draw_synthetic_text(500, 100, "SubLift 硬字幕", "PingFang SC");
    auto res = engine.recognize(img.view());
    REQUIRE_FALSE(res.lines.empty());
    REQUIRE(res.confidence > 0.0);
    REQUIRE((res.text.find("SubLift") != std::string::npos ||
             res.text.find("硬字幕") != std::string::npos ||
             res.text.find("字幕") != std::string::npos));
  }

  SECTION("Gray8 format conversion path") {
    ImageBuffer rgb_img = draw_synthetic_text(400, 100, "SUBLIFT", "Helvetica");
    std::vector<std::uint8_t> gray_data(400U * 100U);
    const std::uint8_t* rgb_ptr = rgb_img.data();
    for (std::size_t i = 0; i < 400U * 100U; ++i) {
      gray_data[i] = static_cast<std::uint8_t>(
          (static_cast<int>(rgb_ptr[i * 3 + 0]) +
           static_cast<int>(rgb_ptr[i * 3 + 1]) +
           static_cast<int>(rgb_ptr[i * 3 + 2])) /
          3);
    }
    ImageView gray_view(gray_data.data(), 400, 100, 400, PixelFormat::Gray8);
    auto res = engine.recognize(gray_view);
    REQUIRE_FALSE(res.lines.empty());
  }

  SECTION("padded stride RGB24") {
    ImageBuffer rgb_img = draw_synthetic_text(400, 100, "SUBLIFT", "Helvetica");
    const int padded_stride = 400 * 3 + 64;
    std::vector<std::uint8_t> padded_data(static_cast<std::size_t>(padded_stride) * 100U, 0);
    for (int y = 0; y < 100; ++y) {
      std::memcpy(padded_data.data() + static_cast<std::size_t>(y) * padded_stride,
                  rgb_img.row(y), 400U * 3U);
    }
    ImageView padded_view(padded_data.data(), 400, 100, padded_stride, PixelFormat::RGB24);
    auto res = engine.recognize(padded_view);
    REQUIRE_FALSE(res.lines.empty());
  }

  SECTION("ultra-thin strip (< 32px) padding & inverse box un-padding") {
    for (int thin_h : {1, 12, 24, 31}) {
      ImageBuffer thin_blank(240, thin_h, PixelFormat::RGB24);
      auto res = engine.recognize(thin_blank.view());
      for (const auto& line : res.lines) {
        CHECK(line.box.y >= 0);
        CHECK(line.box.height >= 1);
        CHECK(line.box.y + line.box.height <= thin_h);
      }
    }

    ImageBuffer full = draw_synthetic_text(400, 60, "SUBLIFT", "Helvetica");
    ImageView strip = full.roi(0, 14, 400, 28);
    auto res = engine.recognize(strip);
    REQUIRE_FALSE(res.lines.empty());
    for (const auto& line : res.lines) {
      CHECK(line.box.x >= 0);
      CHECK(line.box.y >= 0);
      CHECK(line.box.width >= 1);
      CHECK(line.box.height >= 1);
      CHECK(line.box.x + line.box.width <= 400);
      CHECK(line.box.y + line.box.height <= 28);
    }
  }

  SECTION("repeated recognize calls update runtime_stats safely") {
    ImageBuffer img = draw_synthetic_text(300, 80, "TEST", "Helvetica");
    const auto before = engine.runtime_stats();
    for (int i = 0; i < 20; ++i) {
      auto res = engine.recognize(img.view());
      REQUIRE_FALSE(res.lines.empty());
    }
    const auto after = engine.runtime_stats();
    REQUIRE(after.recognize_calls == before.recognize_calls + 20);
    REQUIRE(after.recognized_lines >= before.recognized_lines + 20);
  }

  SECTION("concurrent recognize calls serialized safely by Impl::mutex_") {
    ImageBuffer img = draw_synthetic_text(320, 80, "SUBLIFT", "Helvetica");
    std::promise<void> go_promise;
    std::shared_future<void> go = go_promise.get_future().share();
    constexpr int kThreads = 4;
    constexpr int kIters = 5;

    std::vector<std::thread> threads;
    std::vector<OcrResult> results(static_cast<std::size_t>(kThreads * kIters));
    threads.reserve(kThreads);
    for (int t = 0; t < kThreads; ++t) {
      threads.emplace_back([&, t]() {
        go.wait();
        for (int i = 0; i < kIters; ++i) {
          results[static_cast<std::size_t>(t * kIters + i)] =
              engine.recognize(img.view());
        }
      });
    }
    go_promise.set_value();
    for (auto& th : threads) {
      th.join();
    }
    for (const auto& res : results) {
      REQUIRE_FALSE(res.lines.empty());
    }
  }

  SECTION("bad stride is rejected before incrementing runtime_stats") {
    std::vector<std::uint8_t> small_buf(100, 0);
    // width=100 RGB24 needs stride >= 300; pass stride=10
    ImageView bad_stride_view(small_buf.data(), 100, 10, 10, PixelFormat::RGB24);
    const auto before = engine.runtime_stats();
    const auto res = engine.recognize(bad_stride_view);
    const auto after = engine.runtime_stats();
    REQUIRE(res.lines.empty());
    REQUIRE(res.text.empty());
    REQUIRE(after.recognize_calls == before.recognize_calls);
  }

  SECTION("moved-from engine safely returns empty defaults") {
    AppleVisionOcrEngine src({"en-US"});
    AppleVisionOcrEngine dst = std::move(src);
    ImageBuffer img = draw_synthetic_text(300, 80, "MOVED", "Helvetica");
    const auto moved_res = src.recognize(img.view());
    REQUIRE(moved_res.lines.empty());
    REQUIRE(moved_res.text.empty());
    REQUIRE(src.recognition_languages().empty());
    REQUIRE(src.runtime_stats().recognize_calls == 0);
    REQUIRE(src.runtime_stats().recognized_lines == 0);

    const auto dst_res = dst.recognize(img.view());
    REQUIRE_FALSE(dst_res.lines.empty());
  }

  SECTION("empty recognition_languages still recognizes synthetic English text") {
    AppleVisionOcrEngine auto_lang_engine(std::vector<std::string>{});
    REQUIRE(auto_lang_engine.recognition_languages().empty());
    ImageBuffer img = draw_synthetic_text(400, 100, "SUBLIFT", "Helvetica");
    const auto res = auto_lang_engine.recognize(img.view());
    REQUIRE_FALSE(res.lines.empty());
    REQUIRE(res.confidence > 0.0);
  }
}

TEST_CASE("AppleVisionOcrEngine real inference on parity PNG fixtures", "[apple_vision][adapter]") {
  if (!is_vision_available()) {
    SKIP("Apple Vision framework unavailable at runtime");
  }
  const auto manifest_path = resolve_manifest_path();
  if (manifest_path.empty() || !std::filesystem::exists(manifest_path)) {
    SKIP("Paddle parity fixture manifest not found");
  }

  std::ifstream f(manifest_path);
  REQUIRE(f.is_open());
  nlohmann::json manifest = nlohmann::json::parse(f);
  const auto fixtures_dir = manifest_path.parent_path();

  AppleVisionOcrEngine engine;

  const auto run_fixture = [&](const std::string& filename) -> OcrResult {
    const auto img_path = fixtures_dir / filename;
    REQUIRE(std::filesystem::exists(img_path));
    cv::Mat bgr = cv::imread(img_path.string(), cv::IMREAD_COLOR);
    REQUIRE_FALSE(bgr.empty());
    cv::Mat rgb;
    cv::cvtColor(bgr, rgb, cv::COLOR_BGR2RGB);
    ImageView view(
        rgb.data, rgb.cols, rgb.rows, static_cast<std::int32_t>(rgb.step), PixelFormat::RGB24);
    return engine.recognize(view);
  };

  SECTION("empty.png produces zero lines") {
    const auto res = run_fixture("empty.png");
    REQUIRE(res.lines.empty());
    REQUIRE(res.text.empty());
  }

  SECTION("cjk_single_line.png") {
    const auto res = run_fixture("cjk_single_line.png");
    REQUIRE_FALSE(res.lines.empty());
    REQUIRE(res.confidence > 0.0);
    REQUIRE(res.text.find("硬字幕") != std::string::npos);
  }

  SECTION("latin_single_line.png") {
    const auto res = run_fixture("latin_single_line.png");
    REQUIRE_FALSE(res.lines.empty());
    REQUIRE(res.confidence > 0.0);
    REQUIRE(res.text.find("SubLift") != std::string::npos);
  }

  SECTION("mixed_cjk_latin.png") {
    const auto res = run_fixture("mixed_cjk_latin.png");
    REQUIRE_FALSE(res.lines.empty());
    REQUIRE(res.confidence > 0.0);
    REQUIRE(res.text.find("SubLift") != std::string::npos);
  }

  SECTION("stroke_outlined.png") {
    const auto res = run_fixture("stroke_outlined.png");
    REQUIRE_FALSE(res.lines.empty());
    REQUIRE(res.confidence > 0.0);
    REQUIRE(res.text.find("字幕") != std::string::npos);
  }

  SECTION("adjacent_two_line.png") {
    const auto res = run_fixture("adjacent_two_line.png");
    REQUIRE(res.lines.size() >= 2);
    REQUIRE(res.confidence > 0.0);
    for (const auto& line : res.lines) {
      CHECK(line.box.x >= 0);
      CHECK(line.box.y >= 0);
      CHECK(line.box.width > 0);
      CHECK(line.box.height > 0);
      CHECK(line.box.x + line.box.width <= 960);
      CHECK(line.box.y + line.box.height <= 256);
    }
  }
}

#endif
