#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/mman.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

#include <catch2/catch_test_macros.hpp>
#include <sublift/core/image_buffer.hpp>
#include <sublift/core/image_view.hpp>
#include <sublift/features/dhash.hpp>
#include <sublift/features/geometry.hpp>
#include <sublift/features/mask.hpp>
#include <sublift/features/signature.hpp>
#include <sublift/features/ssim.hpp>
#include <sublift/features/visual_analysis.hpp>

namespace {

sublift::ImageBuffer make_test_image(int width, int height, sublift::PixelFormat format, uint8_t fill_val) {
  sublift::ImageBuffer buf(width, height, format);
  for (int y = 0; y < height; ++y) {
    uint8_t* row = buf.row(y);
    int row_bytes = width * (format == sublift::PixelFormat::RGB24 ? 3 : 1);
    std::memset(row, fill_val, row_bytes);
  }
  return buf;
}

} // namespace

TEST_CASE("dHash computation and hamming distance verification", "[features][dhash]") {
  SECTION("Input validation on invalid hash_size or empty image") {
    auto img = make_test_image(32, 32, sublift::PixelFormat::Gray8, 128);
    sublift::ImageBuffer empty_buf;

    REQUIRE(sublift::features::compute_dhash(empty_buf.view(), 8) == 0ULL);
    REQUIRE(sublift::features::compute_dhash(img.view(), 0) == 0ULL);
    REQUIRE(sublift::features::compute_dhash(img.view(), -1) == 0ULL);
    REQUIRE(sublift::features::compute_dhash(img.view(), 9) == 0ULL);
    REQUIRE(sublift::features::compute_dhash(img.view(), 100) == 0ULL);
  }

  SECTION("Bit ordering Row-Major MSB-first verification") {
    // Resize target is (hash_size + 1, hash_size) = (9, 8).
    // Test that first comparison (y=0, x=0) controls bit 63 (MSB).
    // An image of 9x8 Gray8 where only (0, 1) > (0, 0), and all other (y, x+1) <= (y, x)
    sublift::ImageBuffer test_msb(9, 8, sublift::PixelFormat::Gray8);
    for (int y = 0; y < 8; ++y) {
      uint8_t* row = test_msb.row(y);
      for (int x = 0; x < 9; ++x) {
        row[x] = 100;
      }
    }
    test_msb.row(0)[1] = 200; // only (x=1, y=0) > (x=0, y=0)
    uint64_t hash_msb = sublift::features::compute_dhash(test_msb.view(), 8);
    REQUIRE((hash_msb & (1ULL << 63)) != 0ULL);

    // Test that last comparison (y=7, x=7) controls bit 0 (LSB)
    sublift::ImageBuffer test_lsb(9, 8, sublift::PixelFormat::Gray8);
    for (int y = 0; y < 8; ++y) {
      uint8_t* row = test_lsb.row(y);
      for (int x = 0; x < 9; ++x) {
        row[x] = 100;
      }
    }
    test_lsb.row(7)[8] = 200; // only (x=8, y=7) > (x=7, y=7)
    uint64_t hash_lsb = sublift::features::compute_dhash(test_lsb.view(), 8);
    REQUIRE((hash_lsb & (1ULL << 0)) != 0ULL);
  }

  SECTION("Hamming distance properties") {
    // Identity
    REQUIRE(sublift::features::hamming_distance(0x123456789ABCDEF0ULL, 0x123456789ABCDEF0ULL) == 0);

    // Symmetry
    uint64_t h1 = 0xAAAAAAAAAAAAAAAAULL;
    uint64_t h2 = 0x5555555555555555ULL;
    REQUIRE(sublift::features::hamming_distance(h1, h2) == 64);
    REQUIRE(sublift::features::hamming_distance(h2, h1) == 64);

    // Hand-crafted differences
    REQUIRE(sublift::features::hamming_distance(0x01ULL, 0x02ULL) == 2);
    REQUIRE(sublift::features::hamming_distance(0x0FULL, 0xF0ULL) == 8);

    // Triangle inequality: d(A, C) <= d(A, B) + d(B, C)
    uint64_t a = 0x12345678ULL;
    uint64_t b = 0x9ABCDEF0ULL;
    uint64_t c = 0xFEDCBA98ULL;
    REQUIRE(sublift::features::hamming_distance(a, c) <=
            sublift::features::hamming_distance(a, b) + sublift::features::hamming_distance(b, c));
  }
}

TEST_CASE("Adaptive threshold mask and foreground ratio", "[features][mask]") {
  SECTION("Empty image returns empty buffer and zero ratio") {
    sublift::ImageBuffer empty_buf;
    auto mask = sublift::features::compute_binary_mask(empty_buf.view());
    REQUIRE(mask.empty());
    REQUIRE(sublift::features::compute_foreground_ratio(mask.view()) == 0.0);
  }

  SECTION("Foreground ratio calculation accuracy") {
    // 32x32 = 1024 pixels. Set exactly 256 pixels to 255 -> ratio 0.25
    sublift::ImageBuffer mask(32, 32, sublift::PixelFormat::Gray8);
    std::memset(mask.data(), 0, 1024);
    for (int y = 0; y < 8; ++y) {
      for (int x = 0; x < 32; ++x) {
        mask.row(y)[x] = 255;
      }
    }
    double ratio = sublift::features::compute_foreground_ratio(mask.view());
    REQUIRE(std::abs(ratio - 0.25) < 1e-9);

    // All white mask -> ratio 1.0
    std::memset(mask.data(), 255, 1024);
    REQUIRE(std::abs(sublift::features::compute_foreground_ratio(mask.view()) - 1.0) < 1e-9);
  }

  SECTION("Dynamic block size adaptation") {
    // 128x64 image with ratio 0.08:
    // effective_height = max(64, min(120, 128 / 11 = 11)) = 64
    // block_size = max(3, int(64 * 0.08)) = 5 (odd)
    sublift::ImageBuffer img(128, 64, sublift::PixelFormat::RGB24);
    std::memset(img.data(), 40, 128 * 64 * 3);
    // Draw high-contrast text stroke pattern
    for (int y = 20; y < 44; ++y) {
      uint8_t* row = img.row(y);
      for (int x = 30; x < 98; ++x) {
        if ((x % 10 < 4) || (y % 8 < 3)) {
          row[x * 3 + 0] = 10;
          row[x * 3 + 1] = 10;
          row[x * 3 + 2] = 10;
        } else {
          row[x * 3 + 0] = 220;
          row[x * 3 + 1] = 220;
          row[x * 3 + 2] = 220;
        }
      }
    }
    sublift::features::MaskConfig cfg{.block_size_ratio = 0.08, .adaptive_c = 12.0};
    auto mask = sublift::features::compute_binary_mask(img.view(), cfg);
    REQUIRE(mask.width() == 128);
    REQUIRE(mask.height() == 64);
    double fg = sublift::features::compute_foreground_ratio(mask.view());
    REQUIRE(fg > 0.0);
    REQUIRE(fg < 1.0);
  }

  SECTION("Explicit block size override") {
    sublift::ImageBuffer img(64, 64, sublift::PixelFormat::Gray8);
    std::memset(img.data(), 100, 64 * 64);
    for (int y = 20; y < 40; ++y) {
      for (int x = 20; x < 40; ++x) {
        img.row(y)[x] = 240;
      }
    }
    // Override with even number 6 -> should round up to 7
    sublift::features::MaskConfig cfg{.adaptive_block_size = 6};
    auto mask = sublift::features::compute_binary_mask(img.view(), cfg);
    REQUIRE_FALSE(mask.empty());
  }

  SECTION("Large dimensions (50,000 x 50,000) prevent signed integer overflow") {
#if defined(__unix__) || defined(__APPLE__)
    constexpr std::size_t total_bytes = 50000ULL * 50000ULL;
    void* mem = mmap(nullptr, total_bytes, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
    REQUIRE(mem != MAP_FAILED);
    uint8_t* p = static_cast<uint8_t*>(mem);
    for (int x = 0; x < 1000; ++x) {
      p[x] = 255;
    }
    sublift::ImageView large_view(p, 50000, 50000, 50000, sublift::PixelFormat::Gray8);
    double ratio = sublift::features::compute_foreground_ratio(large_view);
    REQUIRE(ratio >= 0.0);
    REQUIRE(ratio <= 1.0);
    REQUIRE(std::abs(ratio - (1000.0 / 2500000000.0)) < 1e-12);
    munmap(mem, total_bytes);
#elif defined(_WIN32)
    constexpr std::size_t total_bytes = 50000ULL * 50000ULL;
    void* mem = VirtualAlloc(nullptr, total_bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    REQUIRE(mem != nullptr);
    uint8_t* p = static_cast<uint8_t*>(mem);
    for (int x = 0; x < 1000; ++x) {
      p[x] = 255;
    }
    sublift::ImageView large_view(p, 50000, 50000, 50000, sublift::PixelFormat::Gray8);
    double ratio = sublift::features::compute_foreground_ratio(large_view);
    REQUIRE(ratio >= 0.0);
    REQUIRE(ratio <= 1.0);
    REQUIRE(std::abs(ratio - (1000.0 / 2500000000.0)) < 1e-12);
    VirtualFree(mem, 0, MEM_RELEASE);
#endif
  }
}

TEST_CASE("SSIM computation and SsimWorkspace buffer reuse", "[features][ssim]") {
  SECTION("Byte identical fast path") {
    auto a = make_test_image(64, 32, sublift::PixelFormat::Gray8, 150);
    auto b = make_test_image(64, 32, sublift::PixelFormat::Gray8, 150);
    REQUIRE(sublift::features::compute_ssim(a.view(), b.view()) == 1.0);
    REQUIRE(sublift::features::compute_mask_ssim(a.view(), b.view()) == 1.0);
  }

  SECTION("Differing uniform images") {
    auto a = make_test_image(32, 32, sublift::PixelFormat::Gray8, 100);
    auto b = make_test_image(32, 32, sublift::PixelFormat::Gray8, 200);
    double ssim_val = sublift::features::compute_ssim(a.view(), b.view());
    REQUIRE(ssim_val < 1.0);
    REQUIRE(ssim_val > 0.0);
  }

  SECTION("Empty image handling") {
    sublift::ImageBuffer empty1;
    sublift::ImageBuffer empty2;
    auto valid = make_test_image(32, 32, sublift::PixelFormat::Gray8, 100);

    REQUIRE(sublift::features::compute_ssim(empty1.view(), empty2.view()) == 1.0);
    REQUIRE(sublift::features::compute_ssim(empty1.view(), valid.view()) == 0.0);
    REQUIRE(sublift::features::compute_ssim(valid.view(), empty1.view()) == 0.0);
  }

  SECTION("Workspace capacity management and zero-reallocation persistence") {
    sublift::features::SsimWorkspace ws;

    // Allocate for 128x64
    ws.ensure_capacity(64, 128);
    void* initial_ptr = ws.raw_buffer_data();
    REQUIRE(initial_ptr != nullptr);
    std::size_t cap = ws.capacity_elements();
    REQUIRE(cap >= 128 * 64);
    REQUIRE(ws.current_rows() == 64);
    REQUIRE(ws.current_cols() == 128);

    // Transition to smaller dimensions: rows=32, cols=64
    ws.ensure_capacity(32, 64);
    REQUIRE(ws.raw_buffer_data() == initial_ptr);
    REQUIRE(ws.capacity_elements() == cap);
    REQUIRE(ws.current_rows() == 32);
    REQUIRE(ws.current_cols() == 64);

    // Compute multiple times using workspace without reallocating
    auto img1 = make_test_image(64, 32, sublift::PixelFormat::Gray8, 80);
    auto img2 = make_test_image(64, 32, sublift::PixelFormat::Gray8, 120);
    double ssim_ws1 = ws.compute(img1.view(), img2.view());
    REQUIRE(ws.raw_buffer_data() == initial_ptr);

    double ssim_ws2 = sublift::features::compute_ssim(img1.view(), img2.view(), 7, &ws);
    REQUIRE(ws.raw_buffer_data() == initial_ptr);
    REQUIRE(std::abs(ssim_ws1 - ssim_ws2) < 1e-12);

    // Verify numerical equivalence between workspace-enabled and workspace-less calls
    double ssim_no_ws = sublift::features::compute_ssim(img1.view(), img2.view(), 7, nullptr);
    REQUIRE(std::abs(ssim_ws1 - ssim_no_ws) < 1e-12);
  }

  SECTION("Binary mask identity equivalence") {
    // Binary mask where values are in {0, 255}
    const int w = 64;
    const int h = 32;
    sublift::ImageBuffer m1(w, h, sublift::PixelFormat::Gray8);
    sublift::ImageBuffer m2(w, h, sublift::PixelFormat::Gray8);
    std::memset(m1.data(), 0, w * h);
    std::memset(m2.data(), 0, w * h);

    for (int y = 5; y < 25; ++y) {
      for (int x = 10; x < 50; ++x) {
        if ((x + y) % 3 == 0) {
          m1.row(y)[x] = 255;
        }
        if ((x + y) % 4 == 0) {
          m2.row(y)[x] = 255;
        }
      }
    }

    sublift::features::SsimWorkspace ws;
    double ssim_standard = sublift::features::compute_ssim(m1.view(), m2.view(), 7, &ws);
    double ssim_mask_identity = sublift::features::compute_mask_ssim(m1.view(), m2.view(), 7, &ws);

    // Binary mask identity should achieve near bit-exact equivalence (< 1e-9)
    REQUIRE(std::abs(ssim_standard - ssim_mask_identity) < 1e-9);
  }

  SECTION("Workspace move semantics") {
    sublift::features::SsimWorkspace ws1;
    ws1.ensure_capacity(32, 32);
    void* ptr = ws1.raw_buffer_data();
    REQUIRE(ptr != nullptr);

    // Move construct
    sublift::features::SsimWorkspace ws2(std::move(ws1));
    REQUIRE(ws2.raw_buffer_data() == ptr);
    REQUIRE(ws2.current_rows() == 32);
    REQUIRE(ws2.current_cols() == 32);

    // Move assign
    sublift::features::SsimWorkspace ws3;
    ws3 = std::move(ws2);
    REQUIRE(ws3.raw_buffer_data() == ptr);
    REQUIRE(ws3.current_rows() == 32);
  }

  SECTION("Invalid window_size (<= 0 or even) parameter defense returns 0.0") {
    auto a = make_test_image(32, 32, sublift::PixelFormat::Gray8, 100);
    auto b = make_test_image(32, 32, sublift::PixelFormat::Gray8, 200);

    for (int bad_win : {0, -1, -7, 2, 4, 6, 8}) {
      REQUIRE(sublift::features::compute_ssim(a.view(), b.view(), bad_win) == 0.0);
      REQUIRE(sublift::features::compute_mask_ssim(a.view(), b.view(), bad_win) == 0.0);
    }

    // Byte-identical fast path must still reject invalid window_size
    REQUIRE(sublift::features::compute_ssim(a.view(), a.view(), 0) == 0.0);
    REQUIRE(sublift::features::compute_ssim(a.view(), a.view(), -1) == 0.0);
    REQUIRE(sublift::features::compute_ssim(a.view(), a.view(), 4) == 0.0);
    REQUIRE(sublift::features::compute_mask_ssim(a.view(), a.view(), 0) == 0.0);
    REQUIRE(sublift::features::compute_mask_ssim(a.view(), a.view(), -1) == 0.0);
    REQUIRE(sublift::features::compute_mask_ssim(a.view(), a.view(), 4) == 0.0);

    // SsimWorkspace method must also reject invalid window_size
    sublift::features::SsimWorkspace ws;
    REQUIRE(ws.compute(a.view(), b.view(), 0) == 0.0);
    REQUIRE(ws.compute(a.view(), b.view(), -1) == 0.0);
    REQUIRE(ws.compute(a.view(), b.view(), 6) == 0.0);
  }

  SECTION("Wide strip mask SSIM footprint normalization and sector/multiline sensitivity") {
    constexpr int w = 640;
    constexpr int h = 64; // aspect = 10.0 > 6.0
    sublift::ImageBuffer m1(w, h, sublift::PixelFormat::Gray8);
    sublift::ImageBuffer m2(w, h, sublift::PixelFormat::Gray8);
    std::memset(m1.data(), 0, static_cast<std::size_t>(w * h));
    std::memset(m2.data(), 0, static_cast<std::size_t>(w * h));

    for (int y = 22; y < 42; ++y) {
      for (int x = 280; x < 320; ++x) {
        if ((x % 4) < 2) {
          m1.row(y)[x] = 255;
        }
      }
      for (int x = 325; x < 365; ++x) {
        if (((x + 2) % 4) < 2) {
          m2.row(y)[x] = 255;
        }
      }
    }

    const double raw_gray_ssim = sublift::features::compute_ssim(m1.view(), m2.view(), 7, nullptr);
    const double unnorm_mask_ssim =
        sublift::features::compute_mask_ssim(m1.view(), m2.view(), 7, nullptr, 20.0);
    REQUIRE(raw_gray_ssim > 0.92);
    REQUIRE(unnorm_mask_ssim > 0.92);

    sublift::features::SsimWorkspace ws;
    ws.ensure_capacity(h, w);
    void* ptr = ws.raw_buffer_data();
    const std::size_t cap = ws.capacity_elements();

    const double norm_ws = sublift::features::compute_mask_ssim(m1.view(), m2.view(), 7, &ws);
    const double norm_no_ws = sublift::features::compute_mask_ssim(m1.view(), m2.view(), 7, nullptr);
    REQUIRE(norm_ws < 0.90);
    REQUIRE(std::abs(norm_ws - norm_no_ws) < 1e-12);
    REQUIRE(ws.raw_buffer_data() == ptr);
    REQUIRE(ws.capacity_elements() == cap);
    REQUIRE(ws.current_rows() == h);
    REQUIRE(ws.current_cols() == w);

    // Multiline top/bottom split (H = 80 >= 60): identical top line, changed bottom line
    constexpr int multi_h = 80;
    sublift::ImageBuffer ml1(w, multi_h, sublift::PixelFormat::Gray8);
    sublift::ImageBuffer ml2(w, multi_h, sublift::PixelFormat::Gray8);
    std::memset(ml1.data(), 0, static_cast<std::size_t>(w * multi_h));
    std::memset(ml2.data(), 0, static_cast<std::size_t>(w * multi_h));

    for (int y = 12; y < 28; ++y) {
      for (int x = 220; x < 420; ++x) {
        if ((x % 4) < 2) {
          ml1.row(y)[x] = 255;
          ml2.row(y)[x] = 255;
        }
      }
    }
    for (int y = 52; y < 68; ++y) {
      for (int x = 240; x < 330; ++x) {
        if ((x % 4) < 2) {
          ml1.row(y)[x] = 255;
        }
      }
      for (int x = 350; x < 440; ++x) {
        if ((x % 4) < 2) {
          ml2.row(y)[x] = 255;
        }
      }
    }

    const double multi_ssim =
        sublift::features::compute_mask_ssim(ml1.view(), ml2.view(), 7, &ws);
    REQUIRE(multi_ssim < 0.90);
  }
}

TEST_CASE("Geometry corridor and text region validation", "[features][geometry]") {
  const int frame_w = 1920;
  const int frame_h = 1080;

  SECTION("Valid subtitle candidate box") {
    sublift::FrameLocalBox box{100, 800, 600, 60}; // aspect = 10.0, area = 36000 / 2073600 = 0.017
    REQUIRE(sublift::features::is_valid_text_region(box, frame_w, frame_h));
  }

  SECTION("Empty or invalid frame dimensions") {
    sublift::FrameLocalBox box{100, 800, 600, 60};
    REQUIRE_FALSE(sublift::features::is_valid_text_region(box, 0, 1080));
    REQUIRE_FALSE(sublift::features::is_valid_text_region(box, 1920, -10));

    sublift::FrameLocalBox empty_box{0, 0, 0, 0};
    REQUIRE_FALSE(sublift::features::is_valid_text_region(empty_box, frame_w, frame_h));
  }

  SECTION("Out-of-bounds bounding boxes") {
    sublift::FrameLocalBox neg_x{-5, 800, 600, 60};
    REQUIRE_FALSE(sublift::features::is_valid_text_region(neg_x, frame_w, frame_h));

    sublift::FrameLocalBox neg_y{100, -1, 600, 60};
    REQUIRE_FALSE(sublift::features::is_valid_text_region(neg_y, frame_w, frame_h));

    sublift::FrameLocalBox overflow_x{1500, 800, 500, 60}; // x + w = 2000 > 1920
    REQUIRE_FALSE(sublift::features::is_valid_text_region(overflow_x, frame_w, frame_h));

    sublift::FrameLocalBox overflow_y{100, 1050, 600, 60}; // y + h = 1110 > 1080
    REQUIRE_FALSE(sublift::features::is_valid_text_region(overflow_y, frame_w, frame_h));
  }

  SECTION("Aspect ratio constraints") {
    // Too vertical: w=60, h=600 -> aspect = 0.1 < min 1.5
    sublift::FrameLocalBox vertical{100, 200, 60, 600};
    REQUIRE_FALSE(sublift::features::is_valid_text_region(vertical, frame_w, frame_h));

    // Too wide: w=1800, h=40 -> aspect = 45.0 > max 25.0
    sublift::FrameLocalBox too_wide{50, 900, 1800, 40};
    REQUIRE_FALSE(sublift::features::is_valid_text_region(too_wide, frame_w, frame_h));
  }

  SECTION("Area fraction constraint") {
    // Tiny speck: w=10, h=5 -> area = 50 / 2073600 = 0.000024 < 0.001
    sublift::FrameLocalBox tiny{100, 800, 10, 5};
    REQUIRE_FALSE(sublift::features::is_valid_text_region(tiny, frame_w, frame_h));
  }

  SECTION("Extreme coordinate and area dimensions (50,000 x 50,000) prevent signed integer overflow") {
    const int large_w = 50000;
    const int large_h = 50000;
    // Box area = 10000 * 1000 = 10,000,000. Frame area = 2,500,000,000. Fraction = 0.004 >= min 0.001. Aspect = 10.0.
    sublift::FrameLocalBox valid_box{100, 100, 10000, 1000};
    REQUIRE(sublift::features::is_valid_text_region(valid_box, large_w, large_h));

    // Near INT32_MAX coordinates must return false without signed overflow UB
    sublift::FrameLocalBox overflow_coord_x{2000000000, 100, 1000000000, 30};
    REQUIRE_FALSE(sublift::features::is_valid_text_region(overflow_coord_x, large_w, large_h));

    sublift::FrameLocalBox overflow_coord_y{100, 2000000000, 300, 1000000000};
    REQUIRE_FALSE(sublift::features::is_valid_text_region(overflow_coord_y, large_w, large_h));
  }
}

TEST_CASE("Visual analysis pipeline composite", "[features][visual_analysis]") {
  SECTION("Pipeline with RGB24 image") {
    auto img = make_test_image(64, 32, sublift::PixelFormat::RGB24, 40);
    // Add stroke pattern
    for (int y = 8; y < 24; ++y) {
      uint8_t* row = img.row(y);
      for (int x = 8; x < 56; ++x) {
        if ((x % 8 < 3) || (y % 6 < 2)) {
          row[x * 3 + 0] = 10;
          row[x * 3 + 1] = 10;
          row[x * 3 + 2] = 10;
        } else {
          row[x * 3 + 0] = 220;
          row[x * 3 + 1] = 220;
          row[x * 3 + 2] = 220;
        }
      }
    }

    auto analysis = sublift::features::compute_visual_analysis(img.view(), 2500);
    REQUIRE(analysis.signature.timestamp_ms == 2500);
    REQUIRE(analysis.signature.foreground_ratio > 0.0);
    REQUIRE(analysis.signature.foreground_ratio < 1.0);
    REQUIRE(analysis.signature.dhash != 0ULL);
    REQUIRE(analysis.valid_mask.width() == 64);
    REQUIRE(analysis.valid_mask.height() == 32);
    REQUIRE(analysis.valid_mask.format() == sublift::PixelFormat::Gray8);
  }

  SECTION("Pipeline with empty image") {
    sublift::ImageBuffer empty;
    auto analysis = sublift::features::compute_visual_analysis(empty.view(), 1000);
    REQUIRE(analysis.signature.timestamp_ms == 1000);
    REQUIRE(analysis.signature.foreground_ratio == 0.0);
    REQUIRE(analysis.signature.dhash == 0ULL);
    REQUIRE(analysis.valid_mask.empty());
  }

  SECTION("Wide-strip adaptive presence mask isolates centered text from corner/step noise") {
    constexpr int w = 512;
    constexpr int h = 64;
    auto bg_only = make_test_image(w, h, sublift::PixelFormat::RGB24, 35);

    // 1. One-sided scene step edge + corner noise outside [0.18w, 0.82w] corridor -> rejected
    auto noisy = bg_only.clone();
    for (int y = 0; y < h; ++y) {
      auto* row = noisy.row(y);
      for (int x = 0; x < 60; ++x) {
        row[x * 3 + 0] = 210;
        row[x * 3 + 1] = 210;
        row[x * 3 + 2] = 210;
      }
    }
    for (int y = 6; y < 22; ++y) {
      auto* row = noisy.row(y);
      for (int x = 470; x < 502; ++x) {
        if ((x % 4) < 2) {
          row[x * 3 + 0] = 245;
          row[x * 3 + 1] = 245;
          row[x * 3 + 2] = 245;
        }
      }
    }
    auto noise_analysis = sublift::features::compute_visual_analysis(noisy.view(), 3000);
    CHECK(noise_analysis.signature.foreground_ratio == 0.0);

    // 2. Centered dark-outlined white subtitle glyphs inside corridor -> accepted
    auto subtitle_strip = bg_only.clone();
    for (int g = 0; g < 4; ++g) {
      const int gx = 200 + g * 28;
      // Dark outline box (22x24)
      for (int y = 26; y < 50; ++y) {
        auto* row = subtitle_strip.row(y);
        for (int x = gx - 2; x < gx + 20; ++x) {
          row[x * 3 + 0] = 5;
          row[x * 3 + 1] = 5;
          row[x * 3 + 2] = 5;
        }
      }
      // Bright stroke core inside outline
      for (int y = 28; y < 48; ++y) {
        auto* row = subtitle_strip.row(y);
        for (int x = gx; x < gx + 18; ++x) {
          if ((x - gx) % 5 < 3 || (y - 28) % 6 < 2) {
            row[x * 3 + 0] = 250;
            row[x * 3 + 1] = 250;
            row[x * 3 + 2] = 250;
          }
        }
      }
    }
    auto sub_analysis = sublift::features::compute_visual_analysis(subtitle_strip.view(), 3200);
    CHECK(sub_analysis.signature.foreground_ratio >= 0.015);
    CHECK(sub_analysis.signature.dhash != 0ULL);
    CHECK(sub_analysis.valid_mask.width() == w);
    CHECK(sub_analysis.valid_mask.height() == h);
  }
}

TEST_CASE("Sector dHash bit-packing and sector isolation", "[features][dhash]") {
  sublift::ImageBuffer empty;
  CHECK(sublift::features::compute_sector_dhash(empty.view()) == 0ULL);

  auto narrow = make_test_image(9, 16, sublift::PixelFormat::Gray8, 128);
  CHECK(sublift::features::compute_sector_dhash(narrow.view()) == 0ULL);

  // 200x32 Gray8 mask: left=[0, 56), center=[56, 144), right=[144, 200)
  auto base = make_test_image(200, 32, sublift::PixelFormat::Gray8, 0);
  CHECK(sublift::features::compute_sector_dhash(base.view()) == 0ULL);

  auto left_mod = base.clone();
  for (int y = 8; y < 24; ++y) {
    for (int x = 12; x < 28; ++x) left_mod.row(y)[x] = 255;
  }
  const std::uint64_t h_left = sublift::features::compute_sector_dhash(left_mod.view());
  CHECK((h_left >> 40) != 0ULL);
  CHECK(((h_left >> 24) & 0xFFFFULL) == 0ULL);
  CHECK((h_left & 0xFFFFFFULL) == 0ULL);

  auto center_mod = base.clone();
  for (int y = 8; y < 24; ++y) {
    for (int x = 85; x < 115; ++x) center_mod.row(y)[x] = 255;
  }
  const std::uint64_t h_center = sublift::features::compute_sector_dhash(center_mod.view());
  CHECK((h_center >> 40) == 0ULL);
  CHECK(((h_center >> 24) & 0xFFFFULL) != 0ULL);
  CHECK((h_center & 0xFFFFFFULL) == 0ULL);

  auto right_mod = base.clone();
  for (int y = 8; y < 24; ++y) {
    for (int x = 160; x < 180; ++x) right_mod.row(y)[x] = 255;
  }
  const std::uint64_t h_right = sublift::features::compute_sector_dhash(right_mod.view());
  CHECK((h_right >> 40) == 0ULL);
  CHECK(((h_right >> 24) & 0xFFFFULL) == 0ULL);
  CHECK((h_right & 0xFFFFFFULL) != 0ULL);
}

