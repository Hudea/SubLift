#import <CoreGraphics/CoreGraphics.h>
#import <CoreText/CoreText.h>
#import <Foundation/Foundation.h>

#include <catch2/catch_test_macros.hpp>
#include <memory>
#include <string>
#include <vector>

#include "sublift/image.hpp"
#include "sublift/vision.hpp"

using namespace sublift;

#if defined(SUBLIFT_ENABLE_VISION) && SUBLIFT_ENABLE_VISION

namespace {

// Helper: draw text into an ImageBuffer using CoreGraphics with font fallback
static ImageBuffer draw_synthetic_text(
    int width, int height,
    const std::string& text,
    const std::string& font_name_utf8 = "Helvetica") {
  std::vector<std::uint8_t> rgba_data(static_cast<std::size_t>(width) * height * 4, 255);  // white background

  CGColorSpaceRef color_space = CGColorSpaceCreateDeviceRGB();
  const uint32_t bitmap_info =
      static_cast<uint32_t>(kCGImageAlphaPremultipliedLast) | static_cast<uint32_t>(kCGBitmapByteOrder32Big);
  CGContextRef ctx = CGBitmapContextCreate(
      rgba_data.data(), width, height, 8, width * 4, color_space, bitmap_info);

  if (ctx) {
    // Fill white
    CGContextSetRGBFillColor(ctx, 1.0, 1.0, 1.0, 1.0);
    CGContextFillRect(ctx, CGRectMake(0, 0, width, height));

    // Set black text
    CGContextSetRGBFillColor(ctx, 0.0, 0.0, 0.0, 1.0);

    CFStringRef requested_font = CFStringCreateWithCString(kCFAllocatorDefault, font_name_utf8.c_str(), kCFStringEncodingUTF8);
    CTFontRef font = nullptr;
    if (requested_font) {
      font = CTFontCreateWithName(requested_font, 36.0, nullptr);
      CFRelease(requested_font);
    }

    if (!font && font_name_utf8 != "Helvetica") {
      font = CTFontCreateWithName(CFSTR("PingFangSC-Regular"), 36.0, nullptr);
      if (!font) {
        font = CTFontCreateWithName(CFSTR("PingFang-SC-Regular"), 36.0, nullptr);
      }
      if (!font) {
        font = CTFontCreateWithName(CFSTR("STHeitiSC-Light"), 36.0, nullptr);
      }
    }
    if (!font) {
      font = CTFontCreateWithName(CFSTR("Helvetica"), 36.0, nullptr);
    }

    CFStringRef str = CFStringCreateWithCString(kCFAllocatorDefault, text.c_str(), kCFStringEncodingUTF8);

    if (!font || !str) {
      if (font) CFRelease(font);
      if (str) CFRelease(str);
      CGContextRelease(ctx);
      CGColorSpaceRelease(color_space);
      return ImageBuffer(width, height, PixelFormat::RGB24);
    }

    CFStringRef keys[] = {kCTFontAttributeName, kCTForegroundColorFromContextAttributeName};
    CFTypeRef values[] = {font, kCFBooleanTrue};
    CFDictionaryRef attributes = CFDictionaryCreate(
        kCFAllocatorDefault, (const void**)keys, (const void**)values, 2,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);

    CFAttributedStringRef attr_str = CFAttributedStringCreate(kCFAllocatorDefault, str, attributes);
    CTLineRef line = CTLineCreateWithAttributedString(attr_str);

    // Position text in middle
    CGContextSetTextPosition(ctx, 20, height / 2 - 10);
    CTLineDraw(line, ctx);

    CFRelease(line);
    CFRelease(attr_str);
    CFRelease(attributes);
    CFRelease(font);
    CFRelease(str);
    CGContextRelease(ctx);
  }
  CGColorSpaceRelease(color_space);

  // Convert RGBA to RGB24 ImageBuffer
  std::vector<std::uint8_t> rgb_data(static_cast<std::size_t>(width) * height * 3);
  for (int i = 0; i < width * height; ++i) {
    rgb_data[i * 3 + 0] = rgba_data[i * 4 + 0];
    rgb_data[i * 3 + 1] = rgba_data[i * 4 + 1];
    rgb_data[i * 3 + 2] = rgba_data[i * 4 + 2];
  }

  ImageBuffer buf(width, height, PixelFormat::RGB24);
  std::memcpy(buf.data(), rgb_data.data(), rgb_data.size());
  return buf;
}

}  // namespace

TEST_CASE("VisionOcrEngine recognize integration tests", "[vision][integration][ocr]") {
  VisionOcrEngine engine({"zh-Hans", "en-US"});

  SECTION("TC-01: empty and blank image return empty result") {
    ImageView empty_view;
    auto result = engine.recognize(empty_view);
    REQUIRE(result.text.empty());
    REQUIRE(result.confidence == 0.0);
    REQUIRE(result.lines.empty());

    ImageBuffer blank(200, 100, PixelFormat::RGB24);
    auto blank_res = engine.recognize(blank.view());
    REQUIRE(blank_res.lines.empty());
  }

  SECTION("TC-02: synthetic english text image recognition") {
    ImageBuffer img = draw_synthetic_text(400, 100, "SUBLIFT", "Helvetica");
    auto result = engine.recognize(img.view());

    // L0 hard gate: non-empty lines + conf + boxes. Exact Vision text is L4
    // (OS/Vision nondeterministic) — loose match is WARN-only, not ctest fail.
    REQUIRE(!result.lines.empty());
    REQUIRE(result.confidence > 0.0);
    REQUIRE(!result.text.empty());
    REQUIRE(result.lines[0].box.width > 0);
    REQUIRE(result.lines[0].box.height > 0);
    if (result.text.find("SUBLIFT") == std::string::npos &&
        result.text.find("SubLift") == std::string::npos &&
        result.text.find("sublift") == std::string::npos) {
      WARN("EN smoke L4: OCR text did not contain SUBLIFT; got: " << result.text);
    }
  }

  SECTION("TC-03: synthetic chinese text image recognition") {
    ImageBuffer img = draw_synthetic_text(500, 100, "SubLift 硬字幕", "PingFang SC");
    auto result = engine.recognize(img.view());

    REQUIRE(!result.lines.empty());
    REQUIRE(result.confidence > 0.0);
    // Check either SubLift or Chinese characters recognized
    REQUIRE((result.text.find("SubLift") != std::string::npos ||
             result.text.find("硬字幕") != std::string::npos ||
             result.text.find("字幕") != std::string::npos));
  }

  SECTION("TC-04: language configuration and override") {
    VisionOcrEngine engine_default;
    REQUIRE(engine_default.recognition_languages() == std::vector<std::string>{"zh-Hans", "en-US"});

    VisionOcrEngine engine_zh({"zh-Hans"});
    REQUIRE(engine_zh.recognition_languages() == std::vector<std::string>{"zh-Hans"});

    VisionOcrEngine engine_en({"en-US"});
    REQUIRE(engine_en.recognition_languages() == std::vector<std::string>{"en-US"});

    VisionOcrEngine engine_bilingual({"zh-Hans", "en-US"});
    REQUIRE(engine_bilingual.recognition_languages() == std::vector<std::string>{"zh-Hans", "en-US"});
  }

  SECTION("TC-05: non-RGB24 format handling (BGR24 / Gray8)") {
    ImageBuffer rgb_img = draw_synthetic_text(400, 100, "SUBLIFT", "Helvetica");

    // Create BGR24 image
    std::vector<std::uint8_t> bgr_data(400 * 100 * 3);
    const auto* rgb_ptr = rgb_img.data();
    for (int i = 0; i < 400 * 100; ++i) {
      bgr_data[i * 3 + 0] = rgb_ptr[i * 3 + 2];
      bgr_data[i * 3 + 1] = rgb_ptr[i * 3 + 1];
      bgr_data[i * 3 + 2] = rgb_ptr[i * 3 + 0];
    }
    ImageView bgr_view(bgr_data.data(), 400, 100, 400 * 3, PixelFormat::BGR24);
    auto bgr_res = engine.recognize(bgr_view);
    REQUIRE(!bgr_res.lines.empty());

    // Create Gray8 image
    std::vector<std::uint8_t> gray_data(400 * 100);
    for (int i = 0; i < 400 * 100; ++i) {
      gray_data[i] = static_cast<std::uint8_t>(
          (static_cast<int>(rgb_ptr[i * 3]) + rgb_ptr[i * 3 + 1] + rgb_ptr[i * 3 + 2]) / 3);
    }
    ImageView gray_view(gray_data.data(), 400, 100, 400, PixelFormat::Gray8);
    auto gray_res = engine.recognize(gray_view);
    REQUIRE(!gray_res.lines.empty());
  }

  SECTION("TC-06: padded stride image handling") {
    ImageBuffer rgb_img = draw_synthetic_text(400, 100, "SUBLIFT", "Helvetica");
    int padded_stride = 400 * 3 + 64;  // 64 bytes padding at end of each row
    std::vector<std::uint8_t> padded_data(static_cast<std::size_t>(padded_stride) * 100, 0);

    for (int y = 0; y < 100; ++y) {
      std::memcpy(padded_data.data() + y * padded_stride, rgb_img.data() + y * 400 * 3, 400 * 3);
    }

    ImageView padded_view(padded_data.data(), 400, 100, padded_stride, PixelFormat::RGB24);
    auto res = engine.recognize(padded_view);
    REQUIRE(!res.lines.empty());
  }

  SECTION("TC-07: repeated recognize calls for memory leak safety") {
    ImageBuffer img = draw_synthetic_text(300, 80, "TEST", "Helvetica");
    for (int i = 0; i < 20; ++i) {
      auto res = engine.recognize(img.view());
      REQUIRE(!res.lines.empty());
    }
  }
}

#endif
