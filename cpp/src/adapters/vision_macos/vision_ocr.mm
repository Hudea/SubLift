#import <Foundation/Foundation.h>
#import <Vision/Vision.h>
#import <CoreGraphics/CoreGraphics.h>

#include "sublift/adapters/vision.hpp"
#include <cstring>
#include <memory>
#include <vector>

namespace sublift {

namespace {

// CG types are CF-owned (not ARC). unique_ptr + CFRelease keeps ownership explicit
// alongside ARC-managed Vision/Foundation objects (-fobjc-arc on this TU).
struct CGColorSpaceDeleter {
  void operator()(CGColorSpaceRef p) const noexcept { if (p) CGColorSpaceRelease(p); }
};
using UniqueCGColorSpace = std::unique_ptr<std::remove_pointer_t<CGColorSpaceRef>, CGColorSpaceDeleter>;

struct CGDataProviderDeleter {
  void operator()(CGDataProviderRef p) const noexcept { if (p) CGDataProviderRelease(p); }
};
using UniqueCGDataProvider = std::unique_ptr<std::remove_pointer_t<CGDataProviderRef>, CGDataProviderDeleter>;

struct CGImageDeleter {
  void operator()(CGImageRef p) const noexcept { if (p) CGImageRelease(p); }
};
using UniqueCGImage = std::unique_ptr<std::remove_pointer_t<CGImageRef>, CGImageDeleter>;

/// Build DeviceRGB CGImage for Vision.
/// Product/parity path: RGB24 (tight or padded stride). BGR24/Gray8 are best-effort
/// conversions only (Candidate convenience; Oracle uses PIL RGB).
static UniqueCGImage create_cgimage_from_view(const ImageView& image, std::vector<std::uint8_t>& temp_buf) {
  if (image.empty()) return nullptr;

  const std::int32_t width = image.width();
  const std::int32_t height = image.height();
  std::int32_t stride = image.stride_bytes();
  const std::int32_t bpp = (image.format() == PixelFormat::Gray8) ? 1 : 3;

  if (stride < width * bpp) {
    return nullptr;
  }

  const std::uint8_t* pixel_data = image.data();

  if (image.format() != PixelFormat::RGB24 || stride != width * 3) {
    temp_buf.resize(static_cast<std::size_t>(width) * height * 3);
    std::uint8_t* dst = temp_buf.data();
    for (std::int32_t y = 0; y < height; ++y) {
      const std::uint8_t* src_row = image.row(y);
      std::uint8_t* dst_row = dst + static_cast<std::size_t>(y) * width * 3;
      if (image.format() == PixelFormat::RGB24) {
        std::memcpy(dst_row, src_row, static_cast<std::size_t>(width) * 3);
      } else if (image.format() == PixelFormat::BGR24) {
        for (std::int32_t x = 0; x < width; ++x) {
          dst_row[x * 3 + 0] = src_row[x * 3 + 2];
          dst_row[x * 3 + 1] = src_row[x * 3 + 1];
          dst_row[x * 3 + 2] = src_row[x * 3 + 0];
        }
      } else if (image.format() == PixelFormat::Gray8) {
        for (std::int32_t x = 0; x < width; ++x) {
          const std::uint8_t g = src_row[x];
          dst_row[x * 3 + 0] = g;
          dst_row[x * 3 + 1] = g;
          dst_row[x * 3 + 2] = g;
        }
      }
    }
    pixel_data = temp_buf.data();
    stride = width * 3;
  }

  UniqueCGDataProvider provider(CGDataProviderCreateWithData(
      nullptr, pixel_data, static_cast<std::size_t>(stride) * height, nullptr));
  if (!provider) return nullptr;

  UniqueCGColorSpace color_space(CGColorSpaceCreateDeviceRGB());
  if (!color_space) return nullptr;

  UniqueCGImage cg_image(CGImageCreate(
      static_cast<std::size_t>(width),
      static_cast<std::size_t>(height),
      8, 24, static_cast<std::size_t>(stride),
      color_space.get(),
      kCGImageAlphaNone,
      provider.get(),
      nullptr, false, kCGRenderingIntentDefault));

  return cg_image;
}

}  // namespace

struct VisionOcrEngine::Impl {
  std::vector<std::string> recognition_languages;
};

VisionOcrEngine::VisionOcrEngine(std::vector<std::string> recognition_languages)
    : impl_(std::make_unique<Impl>()) {
  impl_->recognition_languages = std::move(recognition_languages);
}

VisionOcrEngine::~VisionOcrEngine() = default;
VisionOcrEngine::VisionOcrEngine(VisionOcrEngine&&) noexcept = default;
VisionOcrEngine& VisionOcrEngine::operator=(VisionOcrEngine&&) noexcept = default;

const std::vector<std::string>& VisionOcrEngine::recognition_languages() const noexcept {
  static const std::vector<std::string> empty_langs;
  return impl_ ? impl_->recognition_languages : empty_langs;
}

OcrResult VisionOcrEngine::recognize(const ImageView& image) {
  if (!impl_ || image.empty()) {
    return OcrResult{};
  }

  // No outer catch(...): only documented empty-result branches return {};
  // unexpected C++ exceptions (e.g. bad_alloc) propagate like Python untimed path.
  @autoreleasepool {
    std::vector<std::uint8_t> temp_buf;
    UniqueCGImage cg_image = create_cgimage_from_view(image, temp_buf);
    if (!cg_image) {
      return OcrResult{};
    }

    // ARC: +1 from alloc is released when locals leave the pool/scope.
    VNImageRequestHandler* handler =
        [[VNImageRequestHandler alloc] initWithCGImage:cg_image.get() options:@{}];
    if (!handler) {
      return OcrResult{};
    }

    VNRecognizeTextRequest* request = [[VNRecognizeTextRequest alloc] init];
    if (!request) {
      return OcrResult{};
    }

    // Do not set recognitionLevel / revision — Python never sets them.
    NSMutableArray<NSString*>* langs =
        [NSMutableArray arrayWithCapacity:impl_->recognition_languages.size()];
    for (const auto& lang : impl_->recognition_languages) {
      if (NSString* ns_lang = [NSString stringWithUTF8String:lang.c_str()]) {
        [langs addObject:ns_lang];
      }
    }
    [request setRecognitionLanguages:langs];

    NSError* error = nil;
    BOOL success = [handler performRequests:@[request] error:&error];
    if (!success || error != nil) {
      return OcrResult{};
    }

    NSArray<VNRecognizedTextObservation*>* observations = [request results];
    if (observations == nil || [observations count] == 0) {
      return OcrResult{};
    }

    const std::int32_t width = image.width();
    const std::int32_t height = image.height();
    std::vector<OcrLine> lines;
    lines.reserve([observations count]);

    for (VNRecognizedTextObservation* obs in observations) {
      NSArray<VNRecognizedText*>* candidates = [obs topCandidates:1];
      if (candidates == nil || [candidates count] == 0) {
        continue;
      }
      VNRecognizedText* candidate = candidates.firstObject;
      NSString* string_val = candidate.string;
      if (string_val == nil) {
        continue;
      }

      const char* utf8_ptr = [string_val UTF8String];
      if (utf8_ptr == nullptr) {
        continue;
      }

      std::string text = utf8_ptr;
      // Python: if not text.strip(): continue (Unicode whitespace)
      if (is_ocr_text_blank(text)) {
        continue;
      }

      double confidence = static_cast<double>(candidate.confidence);
      // Native CGRect always exposes finite doubles; no Python-style unpack TypeError.
      // vision_normalized_box_to_pixel is noexcept and clamps — no whole-image fallback needed.
      CGRect box = obs.boundingBox;
      OcrCropBox pixel_box = vision_normalized_box_to_pixel(
          box.origin.x, box.origin.y, box.size.width, box.size.height, width, height);

      lines.push_back(OcrLine{
          .text = std::move(text),
          .confidence = confidence,
          .box = pixel_box,
      });
    }

    sort_ocr_lines(lines);
    return OcrResult::from_lines(lines);
  }
}

}  // namespace sublift
