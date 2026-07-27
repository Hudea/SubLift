#pragma once

// Shared helpers for Pipeline tests (feat-06202 feed / 06203 ocr_segment /
// 06204 finalize / 06205 golden). Synthetic 128x64 RGB24 frames with a dark
// gray background; band patterns are calibrated against the Python oracle so
// that compute_signature yields deterministic fg_ratio / dHash.
//
// Chromatic bands (R≠B) exercise the feed RGB→BGR quirk: equal-channel gray
// makes R↔B a no-op for dHash; cyan (0,255,255) only crosses presence with
// the Python quirk applied.

#include <cstdint>
#include <vector>

#include "sublift/config.hpp"
#include "sublift/detector.hpp"
#include "sublift/fixed_detector.hpp"
#include "sublift/image.hpp"
#include "sublift/models.hpp"
#include "sublift/pipeline.hpp"

namespace sublift::test {

inline constexpr std::int32_t kW = 128;
inline constexpr std::int32_t kH = 64;
inline constexpr std::uint8_t kBg = 40;
inline constexpr std::uint8_t kFg = 240;

// Deterministic config: SSIM patrol off so changepoint events are pure
// fg_ratio + dHash. (Full default-config behavior is exercised by the 06205
// golden harness.)
inline Config base_cfg() {
  Config cfg{};
  cfg.change_point.enable_ssim_patrol = false;
  return cfg;
}

inline void fill_bg(ImageBuffer& img) {
  for (std::int32_t y = 0; y < kH; ++y) {
    std::uint8_t* r = img.data() + static_cast<std::size_t>(y) * img.stride_bytes();
    for (std::int32_t x = 0; x < kW; ++x) {
      r[x * 3 + 0] = kBg;
      r[x * 3 + 1] = kBg;
      r[x * 3 + 2] = kBg;
    }
  }
}

inline void stamp(ImageBuffer& img, std::int32_t x0, std::int32_t x1,
                  std::int32_t y0, std::int32_t y1, std::uint8_t r = kFg,
                  std::uint8_t g = kFg, std::uint8_t b = kFg) {
  for (std::int32_t y = y0; y < y1; ++y) {
    std::uint8_t* row =
        img.data() + static_cast<std::size_t>(y) * img.stride_bytes();
    for (std::int32_t x = x0; x < x1; ++x) {
      row[x * 3 + 0] = r;
      row[x * 3 + 1] = g;
      row[x * 3 + 2] = b;
    }
  }
}

inline ImageBuffer blank_image() {
  ImageBuffer img(kW, kH, PixelFormat::RGB24);
  fill_bg(img);
  return img;
}

inline Frame empty_frame(std::int64_t ts) {
  return Frame{ts, blank_image()};
}

// subA: band [48,80) x [44,60) - fg=0.0215, dhash=0x00302830.
inline Frame subA(std::int64_t ts) {
  ImageBuffer img = blank_image();
  stamp(img, 48, 80, 44, 60);
  return Frame{ts, std::move(img)};
}

// subB: band [40,88) x [46,58) - fg=0.0273, dhash=0x00604460; ham(subA,subB)=8.
inline Frame subB(std::int64_t ts) {
  ImageBuffer img = blank_image();
  stamp(img, 40, 88, 46, 58);
  return Frame{ts, std::move(img)};
}

// subCyan: same geometry as subA but RGB (0,255,255). With feed RGB→BGR quirk,
// presence fires (fg≈0.0195); without conversion fg stays 0 and no IN event.
// Calibrated vs Python oracle (dump_pipeline chromatic_bgr_quirk_in_out).
inline Frame subCyan(std::int64_t ts) {
  ImageBuffer img = blank_image();
  stamp(img, 48, 80, 44, 60, /*r=*/0, /*g=*/255, /*b=*/255);
  return Frame{ts, std::move(img)};
}

inline FixedRegionDetector full_detector() {
  return FixedRegionDetector(FrameLocalBox{0, 0, kW, kH});
}

inline std::vector<std::int64_t> fallback_ts(const SegmentEvent& ev) {
  std::vector<std::int64_t> out;
  out.reserve(ev.fallback_frames.size());
  for (const auto& f : ev.fallback_frames) {
    out.push_back(f.timestamp_ms);
  }
  return out;
}

// Detector that returns nullopt on the first call, then a full-frame region.
class FirstNullDetector final : public IDetector {
 public:
  std::optional<Region> detect(const Frame&) override {
    if (first_) {
      first_ = false;
      return std::nullopt;
    }
    return Region{FrameLocalBox{0, 0, kW, kH}};
  }

 private:
  bool first_{true};
};

// Detector that never resolves a region (exercises the ocr_segment region-None
// branch without relying on feed).
class NeverDetectDetector final : public IDetector {
 public:
  std::optional<Region> detect(const Frame&) override { return std::nullopt; }
};

}  // namespace sublift::test
