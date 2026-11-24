#import <CoreGraphics/CoreGraphics.h>
#import <Foundation/Foundation.h>
#import <Vision/Vision.h>

#include <sublift/adapters/apple_vision/apple_vision_geometry.hpp>
#include <sublift/adapters/apple_vision/apple_vision_ocr.hpp>
#include <sublift/adapters/apple_vision/apple_vision_stage_timing.hpp>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <type_traits>
#include <vector>

namespace sublift::adapters::apple_vision {

namespace {

// CG types are CF-owned (not ARC). unique_ptr + CFRelease keeps ownership explicit
// alongside ARC-managed Vision/Foundation objects (-fobjc-arc on this TU).
struct CGColorSpaceDeleter {
  void operator()(CGColorSpaceRef p) const noexcept {
    if (p) {
      CGColorSpaceRelease(p);
    }
  }
};
using UniqueCGColorSpace =
    std::unique_ptr<std::remove_pointer_t<CGColorSpaceRef>, CGColorSpaceDeleter>;

struct CGDataProviderDeleter {
  void operator()(CGDataProviderRef p) const noexcept {
    if (p) {
      CGDataProviderRelease(p);
    }
  }
};
using UniqueCGDataProvider =
    std::unique_ptr<std::remove_pointer_t<CGDataProviderRef>, CGDataProviderDeleter>;

struct CGImageDeleter {
  void operator()(CGImageRef p) const noexcept {
    if (p) {
      CGImageRelease(p);
    }
  }
};
using UniqueCGImage =
    std::unique_ptr<std::remove_pointer_t<CGImageRef>, CGImageDeleter>;

/// Build DeviceRGB CGImage for Vision from RGB24 or Gray8 ImageView.
/// Zero-copy when RGB24 and height >= kMinVisionStripHeight.
/// When height < 32 (ultra-thin subtitle strip crop), pads vertically to 48 px
/// with corner-pixel background fill so Vision's text detector does not drop
/// edge-touching glyphs.
UniqueCGImage create_cgimage_from_view(
    const ImageView& image,
    std::vector<std::uint8_t>& temp_buf,
    std::int32_t* out_padded_height,
    std::int32_t* out_pad_top) {
  if (image.empty()) {
    return nullptr;
  }
  if (image.format() != PixelFormat::RGB24 &&
      image.format() != PixelFormat::Gray8) {
    return nullptr;
  }

  const std::int32_t width = image.width();
  const std::int32_t height = image.height();
  std::int32_t stride = image.stride_bytes();
  const std::int32_t bpp = (image.format() == PixelFormat::Gray8) ? 1 : 3;

  if (stride < width * bpp) {
    return nullptr;
  }

  constexpr std::int32_t kMinVisionStripHeight = 32;
  constexpr std::int32_t kTargetPaddedStripHeight = 48;
  const std::int32_t padded_height =
      height < kMinVisionStripHeight ? kTargetPaddedStripHeight : height;
  const std::int32_t pad_top = (padded_height - height) / 2;
  if (out_padded_height) {
    *out_padded_height = padded_height;
  }
  if (out_pad_top) {
    *out_pad_top = pad_top;
  }

  const std::uint8_t* pixel_data = image.data();

  if (image.format() != PixelFormat::RGB24 || pad_top > 0) {
    const std::size_t row_bytes = static_cast<std::size_t>(width) * 3U;
    temp_buf.resize(row_bytes * static_cast<std::size_t>(padded_height));
    std::uint8_t* dst = temp_buf.data();
    for (std::int32_t y = 0; y < height; ++y) {
      const std::uint8_t* src_row = image.row(y);
      std::uint8_t* dst_row =
          dst + static_cast<std::size_t>(y + pad_top) * row_bytes;
      if (image.format() == PixelFormat::RGB24) {
        std::memcpy(dst_row, src_row, row_bytes);
      } else {
        for (std::int32_t x = 0; x < width; ++x) {
          const std::uint8_t g = src_row[x];
          dst_row[x * 3 + 0] = g;
          dst_row[x * 3 + 1] = g;
          dst_row[x * 3 + 2] = g;
        }
      }
    }
    if (pad_top > 0) {
      const std::uint8_t* first_row =
          dst + static_cast<std::size_t>(pad_top) * row_bytes;
      const std::uint8_t* last_row =
          dst + static_cast<std::size_t>(pad_top + height - 1) * row_bytes;
      for (std::int32_t x = 0; x < width; ++x) {
        dst[x * 3 + 0] = first_row[0];
        dst[x * 3 + 1] = first_row[1];
        dst[x * 3 + 2] = first_row[2];
      }
      for (std::int32_t y = 1; y < pad_top; ++y) {
        std::memcpy(dst + static_cast<std::size_t>(y) * row_bytes, dst, row_bytes);
      }
      if (pad_top + height < padded_height) {
        std::uint8_t* bottom_first =
            dst + static_cast<std::size_t>(pad_top + height) * row_bytes;
        for (std::int32_t x = 0; x < width; ++x) {
          bottom_first[x * 3 + 0] = last_row[0];
          bottom_first[x * 3 + 1] = last_row[1];
          bottom_first[x * 3 + 2] = last_row[2];
        }
        for (std::int32_t y = pad_top + height + 1; y < padded_height; ++y) {
          std::memcpy(dst + static_cast<std::size_t>(y) * row_bytes,
                      bottom_first, row_bytes);
        }
      }
    }
    pixel_data = temp_buf.data();
    stride = width * 3;
  }

  const std::size_t data_bytes =
      static_cast<std::size_t>(stride) * static_cast<std::size_t>(padded_height - 1) +
      static_cast<std::size_t>(width) * 3U;
  UniqueCGDataProvider provider(CGDataProviderCreateWithData(
      nullptr, pixel_data, data_bytes, nullptr));
  if (!provider) {
    return nullptr;
  }

  UniqueCGColorSpace color_space(CGColorSpaceCreateDeviceRGB());
  if (!color_space) {
    return nullptr;
  }

  UniqueCGImage cg_image(CGImageCreate(
      static_cast<std::size_t>(width),
      static_cast<std::size_t>(padded_height),
      8, 24, static_cast<std::size_t>(stride),
      color_space.get(),
      kCGImageAlphaNone,
      provider.get(),
      nullptr, false, kCGRenderingIntentDefault));

  return cg_image;
}

}  // namespace

struct AppleVisionOcrEngine::Impl {
  mutable std::mutex mutex_;
  std::vector<std::string> recognition_languages;
  std::vector<std::uint8_t> temp_buf;
  AppleVisionRuntimeStats runtime_stats;
};

AppleVisionOcrEngine::AppleVisionOcrEngine(AppleVisionOptions options)
    : AppleVisionOcrEngine(std::move(options.recognition_languages)) {}

AppleVisionOcrEngine::AppleVisionOcrEngine(std::vector<std::string> recognition_languages)
    : impl_(std::make_unique<Impl>()) {
  impl_->recognition_languages = std::move(recognition_languages);
}

AppleVisionOcrEngine::~AppleVisionOcrEngine() = default;
AppleVisionOcrEngine::AppleVisionOcrEngine(AppleVisionOcrEngine&&) noexcept = default;
AppleVisionOcrEngine& AppleVisionOcrEngine::operator=(AppleVisionOcrEngine&&) noexcept = default;

std::string AppleVisionOcrEngine::engine_name() const {
  return "apple_vision";
}

const std::vector<std::string>& AppleVisionOcrEngine::recognition_languages() const noexcept {
  static const std::vector<std::string> empty_langs;
  return impl_ ? impl_->recognition_languages : empty_langs;
}

AppleVisionRuntimeStats AppleVisionOcrEngine::runtime_stats() const noexcept {
  if (!impl_) {
    return AppleVisionRuntimeStats{};
  }
  std::lock_guard<std::mutex> lock(impl_->mutex_);
  return impl_->runtime_stats;
}

OcrResult AppleVisionOcrEngine::recognize(const ImageView& image) {
  if (!impl_ || image.empty()) {
    return OcrResult{};
  }

  std::lock_guard<std::mutex> lock(impl_->mutex_);

  @autoreleasepool {
    std::int32_t padded_height = image.height();
    std::int32_t pad_top = 0;
    UniqueCGImage cg_image;
    {
      StageTimer timer(stage_registry(), "apple_vision.cgimage");
      cg_image = create_cgimage_from_view(
          image, impl_->temp_buf, &padded_height, &pad_top);
    }
    if (!cg_image) {
      return OcrResult{};
    }

    ++impl_->runtime_stats.recognize_calls;

    StageTimer rec_timer(stage_registry(), "apple_vision.recognize");

    NSMutableArray<NSString*>* langs =
        [NSMutableArray arrayWithCapacity:impl_->recognition_languages.size()];
    for (const auto& lang : impl_->recognition_languages) {
      if (NSString* ns_lang = [NSString stringWithUTF8String:lang.c_str()]) {
        [langs addObject:ns_lang];
      }
    }

    NSArray<VNRecognizedTextObservation*>* observations = nil;
    constexpr int kMaxVisionAttempts = 2;
    for (int attempt = 0; attempt < kMaxVisionAttempts; ++attempt) {
      @autoreleasepool {
        VNImageRequestHandler* handler =
            [[VNImageRequestHandler alloc] initWithCGImage:cg_image.get() options:@{}];
        if (!handler) {
          return OcrResult{};
        }

        VNRecognizeTextRequest* request = [[VNRecognizeTextRequest alloc] init];
        if (!request) {
          return OcrResult{};
        }
        [request setRecognitionLanguages:langs];

        NSError* error = nil;
        BOOL success = [handler performRequests:@[request] error:&error];
        if (success && error == nil) {
          observations = [request results];
          break;
        }
        const char* desc = error ? [[error localizedDescription] UTF8String] : "unknown error";
        std::fprintf(stderr, "[apple_vision] VNImageRequestHandler failed (attempt %d/%d): %s\n",
                     attempt + 1, kMaxVisionAttempts, desc ? desc : "unknown error");
        if (attempt + 1 >= kMaxVisionAttempts) {
          return OcrResult{};
        }
      }
    }

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
      if (is_ocr_text_blank(text)) {
        continue;
      }

      const double confidence = static_cast<double>(candidate.confidence);
      const CGRect box = obs.boundingBox;
      OcrCropBox pixel_box = vision_normalized_box_to_pixel(
          box.origin.x, box.origin.y, box.size.width, box.size.height,
          width, padded_height);
      if (pad_top > 0) {
        const std::int32_t max_y0 = std::max<std::int32_t>(0, height - 1);
        const std::int32_t y0 =
            std::clamp<std::int32_t>(pixel_box.y - pad_top, 0, max_y0);
        const std::int32_t y1 = std::clamp<std::int32_t>(
            pixel_box.y + pixel_box.height - pad_top, y0 + 1, height);
        pixel_box = clamp_ocr_box(
            pixel_box.x, y0, pixel_box.width, y1 - y0, width, height);
      }

      lines.push_back(OcrLine{
          .text = std::move(text),
          .confidence = confidence,
          .box = pixel_box,
      });
    }

    sort_ocr_lines(lines);
    impl_->runtime_stats.recognized_lines += lines.size();
    return OcrResult::from_lines(lines);
  }
}

}  // namespace sublift::adapters::apple_vision
