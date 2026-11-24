#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <random>
#include <vector>

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

sublift::ImageBuffer make_pattern_image(int width, int height, sublift::PixelFormat format, uint32_t seed) {
  sublift::ImageBuffer buf(width, height, format);
  std::mt19937 rng(seed);
  const int bpp = (format == sublift::PixelFormat::RGB24) ? 3 : 1;
  for (int y = 0; y < height; ++y) {
    uint8_t* row = buf.row(y);
    for (int x = 0; x < width * bpp; ++x) {
      row[x] = static_cast<uint8_t>(rng() & 0xFF);
    }
  }
  return buf;
}

sublift::ImageBuffer make_uniform_image(int width, int height, sublift::PixelFormat format, uint8_t val) {
  sublift::ImageBuffer buf(width, height, format);
  const int bpp = (format == sublift::PixelFormat::RGB24) ? 3 : 1;
  for (int y = 0; y < height; ++y) {
    std::memset(buf.row(y), val, static_cast<size_t>(width * bpp));
  }
  return buf;
}

// Independent Reference Oracle for dHash on a 9x8 grid
uint64_t oracle_dhash_9x8(const uint8_t* data, int stride) {
  uint64_t hash = 0;
  for (int y = 0; y < 8; ++y) {
    const uint8_t* row = data + y * stride;
    for (int x = 0; x < 8; ++x) {
      uint64_t bit = (row[x + 1] > row[x]) ? 1ULL : 0ULL;
      hash = (hash << 1) | bit;
    }
  }
  return hash;
}

// Independent Reference Oracle for Hamming Distance
int oracle_hamming_distance(uint64_t a, uint64_t b) {
  uint64_t diff = a ^ b;
  int count = 0;
  while (diff > 0) {
    count += static_cast<int>(diff & 1ULL);
    diff >>= 1;
  }
  return count;
}

} // namespace

// ============================================================================
// 1. SsimWorkspace Memory Reuse & Stress Harness
// ============================================================================
TEST_CASE("SsimWorkspace memory reuse under extreme multi-size stress", "[stress][ssim][workspace]") {
  sublift::features::SsimWorkspace ws;

  SECTION("Initial unallocated state") {
    CHECK(ws.capacity_elements() == 0);
    CHECK(ws.raw_buffer_data() == nullptr);
    CHECK(ws.current_rows() == 0);
    CHECK(ws.current_cols() == 0);
  }

  SECTION("Monotonic capacity expansion and pointer stability once peaked") {
    // Peak capacity at 1280x720 = 921,600 elements (~169 MB double storage)
    const int peak_w = 1280;
    const int peak_h = 720;
    ws.ensure_capacity(peak_h, peak_w);

    void* peak_ptr = ws.raw_buffer_data();
    REQUIRE(peak_ptr != nullptr);
    size_t peak_cap = ws.capacity_elements();
    REQUIRE(peak_cap >= static_cast<size_t>(peak_w * peak_h));

    // Stress: 300 iterations across diverse sizes all <= peak_cap
    // Include tiny, small, medium, wide, tall, and non-standard sizes
    const std::vector<std::pair<int, int>> sizes = {
        {16, 16},    {32, 32},    {64, 32},    {128, 64},   {256, 144},
        {320, 240},  {640, 360},  {800, 600},  {1280, 720}, {720, 1280},
        {100, 500},  {500, 100},  {33, 97},    {999, 500},  {480, 640},
        {128, 64},   {16, 16},    {640, 360},  {1280, 720}, {64, 64},
    };

    std::mt19937 rng(42);
    for (int iter = 0; iter < 100; ++iter) {
      const auto& [w, h] = sizes[static_cast<size_t>(iter) % sizes.size()];
      auto img1 = make_pattern_image(w, h, sublift::PixelFormat::Gray8, rng());
      auto img2 = make_pattern_image(w, h, sublift::PixelFormat::Gray8, rng());

      double ssim_ws = ws.compute(img1.view(), img2.view(), 7);

      // CRITICAL ASSERTION: Zero reallocation once peak capacity reached
      REQUIRE(ws.raw_buffer_data() == peak_ptr);
      REQUIRE(ws.capacity_elements() == peak_cap);
      REQUIRE(ws.current_rows() == h);
      REQUIRE(ws.current_cols() == w);

      // Verify numerical validity
      REQUIRE(ssim_ws >= -1.0);
      REQUIRE(ssim_ws <= 1.0);

      // Spot check numerical equivalence against workspace-less compute every 10 iterations
      if (iter % 10 == 0) {
        double ssim_direct = sublift::features::compute_ssim(img1.view(), img2.view(), 7, nullptr);
        REQUIRE(std::abs(ssim_ws - ssim_direct) < 1e-11);
      }
    }
  }

  SECTION("Interleaved compute, compute_mask_ssim, and external calls") {
    ws.ensure_capacity(64, 128);
    void* initial_ptr = ws.raw_buffer_data();
    const size_t cap = ws.capacity_elements();

    auto m1 = make_uniform_image(128, 64, sublift::PixelFormat::Gray8, 0);
    auto m2 = make_uniform_image(128, 64, sublift::PixelFormat::Gray8, 255);
    // Draw some binary patterns
    for (int y = 10; y < 50; ++y) {
      for (int x = 20; x < 100; ++x) {
        m1.row(y)[x] = 255;
        if ((x + y) % 2 == 0) {
          m2.row(y)[x] = 255;
        }
      }
    }

    for (int i = 0; i < 50; ++i) {
      double ssim1 = sublift::features::compute_mask_ssim(m1.view(), m2.view(), 7, &ws);
      REQUIRE(ws.raw_buffer_data() == initial_ptr);
      REQUIRE(ws.capacity_elements() == cap);

      double ssim2 = ws.compute(m1.view(), m2.view(), 7);
      REQUIRE(ws.raw_buffer_data() == initial_ptr);

      double ssim3 = sublift::features::compute_ssim(m1.view(), m2.view(), 7, &ws);
      REQUIRE(ws.raw_buffer_data() == initial_ptr);

      // Mask SSIM fast path should closely match standard SSIM
      REQUIRE(std::abs(ssim1 - ssim3) < 1e-8);
      REQUIRE(std::abs(ssim2 - ssim3) < 1e-12);
    }
  }

  SECTION("Edge and invalid capacity requests do not corrupt workspace") {
    ws.ensure_capacity(32, 32);
    void* ptr = ws.raw_buffer_data();
    size_t cap = ws.capacity_elements();
    REQUIRE(ptr != nullptr);

    // Call with zeros or negative
    ws.ensure_capacity(0, 0);
    CHECK(ws.raw_buffer_data() == ptr);
    CHECK(ws.capacity_elements() == cap);

    ws.ensure_capacity(-10, 20);
    CHECK(ws.raw_buffer_data() == ptr);
    CHECK(ws.capacity_elements() == cap);

    ws.ensure_capacity(20, -5);
    CHECK(ws.raw_buffer_data() == ptr);
    CHECK(ws.capacity_elements() == cap);
  }

  SECTION("Mismatched image dimensions handling") {
    // Test img1 != img2 size: compute should resize img2 to img1 and not crash
    auto img1 = make_uniform_image(64, 32, sublift::PixelFormat::Gray8, 100);
    auto img2 = make_uniform_image(128, 64, sublift::PixelFormat::Gray8, 100);

    double ssim_diff = ws.compute(img1.view(), img2.view(), 7);
    REQUIRE(ssim_diff > 0.99); // Uniform 100 resized to 100 is identical
  }

  SECTION("2000 high-frequency multi-size allocations and computations leak check") {
    // Initialize workspace to 1920x1080
    ws.ensure_capacity(1080, 1920);
    void* fixed_ptr = ws.raw_buffer_data();
    size_t fixed_cap = ws.capacity_elements();
    REQUIRE(fixed_ptr != nullptr);

    std::mt19937 rng(777);
    const int num_iters = 2000;
    for (int i = 0; i < num_iters; ++i) {
      // Choose pseudorandom dimensions between 16 and 256
      int w = 16 + static_cast<int>(rng() % 240);
      int h = 16 + static_cast<int>(rng() % 240);

      auto a = make_pattern_image(w, h, sublift::PixelFormat::Gray8, rng());
      auto b = make_pattern_image(w, h, sublift::PixelFormat::Gray8, rng());

      double s = ws.compute(a.view(), b.view(), 7);
      REQUIRE(s >= -1.0);
      REQUIRE(s <= 1.0);

      // Verify that the memory buffer NEVER reallocated
      REQUIRE(ws.raw_buffer_data() == fixed_ptr);
      REQUIRE(ws.capacity_elements() == fixed_cap);
    }
  }

  SECTION("Cross-language SSIM parity with independent Python oracle") {
    // Locate fixture path
    const std::string fixture_sub = "benchmark/parity/fixtures/signature/subtitle.rgb";
    const std::string fixture_sub_b = "benchmark/parity/fixtures/signature/subtitle_b.rgb";
    std::filesystem::path root_dir;
#ifdef SUBLIFT_PROJECT_SOURCE_DIR
    if (std::filesystem::exists(std::filesystem::path(SUBLIFT_PROJECT_SOURCE_DIR) / fixture_sub)) {
      root_dir = SUBLIFT_PROJECT_SOURCE_DIR;
    }
#endif
    if (root_dir.empty()) {
      std::filesystem::path cur = std::filesystem::current_path();
      for (int depth = 0; depth < 5; ++depth) {
        if (std::filesystem::exists(cur / fixture_sub)) {
          root_dir = cur;
          break;
        }
        if (cur.has_parent_path() && cur.parent_path() != cur) {
          cur = cur.parent_path();
        } else {
          break;
        }
      }
    }
    const std::string path1 = (root_dir.empty() ? fixture_sub : (root_dir / fixture_sub).string());
    const std::string path2 = (root_dir.empty() ? fixture_sub_b : (root_dir / fixture_sub_b).string());
    FILE* f1 = std::fopen(path1.c_str(), "rb");
    FILE* f2 = std::fopen(path2.c_str(), "rb");
    REQUIRE(f1 != nullptr);
    REQUIRE(f2 != nullptr);

    sublift::ImageBuffer b1(128, 64, sublift::PixelFormat::RGB24);
    sublift::ImageBuffer b2(128, 64, sublift::PixelFormat::RGB24);
    size_t r1 = std::fread(b1.data(), 1, 128 * 64 * 3, f1);
    size_t r2 = std::fread(b2.data(), 1, 128 * 64 * 3, f2);
    std::fclose(f1);
    std::fclose(f2);
    REQUIRE(r1 == 128 * 64 * 3);
    REQUIRE(r2 == 128 * 64 * 3);

    double cpp_ssim = sublift::features::compute_ssim(b1.view(), b2.view(), 7);
    const double py_reference = 0.891312220053;
    // Cross-language floating point equivalence within 1e-10
    REQUIRE(std::abs(cpp_ssim - py_reference) < 1e-10);
  }
}

// ============================================================================
// 2. dHash & Hamming Distance: Oracle Comparison & Bit Stability
// ============================================================================
TEST_CASE("dHash independent oracle comparison and transformation stability", "[stress][features][dhash]") {
  SECTION("Oracle comparison over 1000 randomized 9x8 images") {
    std::mt19937 rng(1337);
    for (int trial = 0; trial < 1000; ++trial) {
      sublift::ImageBuffer buf(9, 8, sublift::PixelFormat::Gray8);
      for (int y = 0; y < 8; ++y) {
        uint8_t* row = buf.row(y);
        for (int x = 0; x < 9; ++x) {
          row[x] = static_cast<uint8_t>(rng() & 0xFF);
        }
      }

      uint64_t oracle_hash = oracle_dhash_9x8(buf.data(), buf.stride_bytes());
      uint64_t actual_hash = sublift::features::compute_dhash(buf.view(), 8);

      REQUIRE(actual_hash == oracle_hash);
    }
  }

  SECTION("Hamming distance against independent oracle") {
    std::mt19937_64 rng(2026);
    for (int trial = 0; trial < 1000; ++trial) {
      uint64_t a = rng();
      uint64_t b = rng();

      int oracle_dist = oracle_hamming_distance(a, b);
      int actual_dist = sublift::features::hamming_distance(a, b);

      REQUIRE(actual_dist == oracle_dist);
    }
  }

  SECTION("Bit stability: Exact single-bit perturbations") {
    // Start with uniform 9x8 image (hash == 0)
    sublift::ImageBuffer base(9, 8, sublift::PixelFormat::Gray8);
    std::memset(base.data(), 100, 9 * 8);
    REQUIRE(sublift::features::compute_dhash(base.view(), 8) == 0ULL);

    // Turn ON exactly one bit (y, x) by making base.row(y)[x+1] > base.row(y)[x]
    for (int y = 0; y < 8; ++y) {
      for (int x = 0; x < 8; ++x) {
        sublift::ImageBuffer test_img(9, 8, sublift::PixelFormat::Gray8);
        std::memcpy(test_img.data(), base.data(), 9 * 8);
        test_img.row(y)[x + 1] = 200;

        uint64_t hash = sublift::features::compute_dhash(test_img.view(), 8);

        // Bit position: Row-Major MSB-First
        // y=0, x=0 -> bit 63
        // y=7, x=7 -> bit 0
        int bit_index = 63 - (y * 8 + x);
        uint64_t expected_hash = (1ULL << bit_index);

        REQUIRE(hash == expected_hash);
        REQUIRE(sublift::features::hamming_distance(0ULL, hash) == 1);
      }
    }
  }

  SECTION("Inversion stability property") {
    // If adjacent pixels are strictly ordered, inverting the image (255 - pixel)
    // strictly reverses the inequality, yielding bitwise NOT (~hash).
    sublift::ImageBuffer img(9, 8, sublift::PixelFormat::Gray8);
    sublift::ImageBuffer inv_img(9, 8, sublift::PixelFormat::Gray8);
    // Construct strictly monotonically increasing/decreasing alternating rows
    for (int y = 0; y < 8; ++y) {
      for (int x = 0; x < 9; ++x) {
        uint8_t val = static_cast<uint8_t>((y % 2 == 0) ? (x * 20 + 10) : (200 - x * 20));
        img.row(y)[x] = val;
        inv_img.row(y)[x] = static_cast<uint8_t>(255 - val);
      }
    }

    uint64_t hash = sublift::features::compute_dhash(img.view(), 8);
    uint64_t inv_hash = sublift::features::compute_dhash(inv_img.view(), 8);

    REQUIRE(inv_hash == ~hash);
    REQUIRE(sublift::features::hamming_distance(hash, inv_hash) == 64);
  }

  SECTION("Vertical flip row-permutation property") {
    // Flipping vertically maps row y to row (7 - y)
    sublift::ImageBuffer img(9, 8, sublift::PixelFormat::Gray8);
    sublift::ImageBuffer vflip(9, 8, sublift::PixelFormat::Gray8);
    std::mt19937 rng(999);
    for (int y = 0; y < 8; ++y) {
      for (int x = 0; x < 9; ++x) {
        uint8_t val = static_cast<uint8_t>(rng() & 0xFF);
        img.row(y)[x] = val;
        vflip.row(7 - y)[x] = val;
      }
    }

    uint64_t h1 = sublift::features::compute_dhash(img.view(), 8);
    uint64_t h2 = sublift::features::compute_dhash(vflip.view(), 8);

    // Each 8-bit row byte should be permuted: row y in h1 becomes row (7-y) in h2
    for (int y = 0; y < 8; ++y) {
      uint8_t row_byte_1 = static_cast<uint8_t>((h1 >> ((7 - y) * 8)) & 0xFF);
      uint8_t row_byte_2 = static_cast<uint8_t>((h2 >> (y * 8)) & 0xFF);
      REQUIRE(row_byte_1 == row_byte_2);
    }
  }
}

// ============================================================================
// 3. Boundary Conditions & Extreme Aspect Ratios
// ============================================================================
TEST_CASE("Boundary conditions, degenerate sizes and extreme aspect ratios", "[stress][features][boundary]") {
  SECTION("1x1 degenerate images") {
    auto img_gray = make_uniform_image(1, 1, sublift::PixelFormat::Gray8, 128);
    auto img_rgb = make_uniform_image(1, 1, sublift::PixelFormat::RGB24, 128);

    // dHash on 1x1
    uint64_t h_gray = sublift::features::compute_dhash(img_gray.view());
    CHECK(h_gray == 0ULL);
    uint64_t h_rgb = sublift::features::compute_dhash(img_rgb.view());
    CHECK(h_rgb == 0ULL);

    // Binary mask on 1x1
    auto mask_gray = sublift::features::compute_binary_mask(img_gray.view());
    CHECK(mask_gray.width() == 1);
    CHECK(mask_gray.height() == 1);

    auto mask_rgb = sublift::features::compute_binary_mask(img_rgb.view());
    CHECK(mask_rgb.width() == 1);
    CHECK(mask_rgb.height() == 1);

    // Foreground ratio on 1x1
    double fg_ratio = sublift::features::compute_foreground_ratio(mask_gray.view());
    CHECK((fg_ratio == 0.0 || fg_ratio == 1.0));

    // SSIM on 1x1 (identical images should return 1.0 via fast-path)
    double ssim_1x1 = sublift::features::compute_ssim(img_gray.view(), img_gray.view());
    CHECK(ssim_1x1 == 1.0);

    // Visual analysis composite on 1x1
    auto analysis = sublift::features::compute_visual_analysis(img_gray.view(), 500);
    CHECK(analysis.signature.timestamp_ms == 500);
    CHECK(analysis.signature.dhash == 0ULL);
    CHECK(analysis.valid_mask.width() == 1);
    CHECK(analysis.valid_mask.height() == 1);
  }

  SECTION("Extreme aspect ratio: 1x1000 (ultra tall line)") {
    auto img_tall_gray = make_uniform_image(1, 1000, sublift::PixelFormat::Gray8, 150);
    auto img_tall_rgb = make_uniform_image(1, 1000, sublift::PixelFormat::RGB24, 150);

    CHECK(sublift::features::compute_dhash(img_tall_gray.view()) == 0ULL);
    CHECK(sublift::features::compute_dhash(img_tall_rgb.view()) == 0ULL);

    auto mask_tall = sublift::features::compute_binary_mask(img_tall_gray.view());
    CHECK(mask_tall.width() == 1);
    CHECK(mask_tall.height() == 1000);

    double fg = sublift::features::compute_foreground_ratio(mask_tall.view());
    CHECK(fg >= 0.0);
    CHECK(fg <= 1.0);

    double ssim_tall = sublift::features::compute_ssim(img_tall_gray.view(), img_tall_gray.view());
    CHECK(ssim_tall == 1.0);
  }

  SECTION("Extreme aspect ratio: 1000x1 (ultra wide line)") {
    auto img_wide_gray = make_uniform_image(1000, 1, sublift::PixelFormat::Gray8, 150);
    auto img_wide_rgb = make_uniform_image(1000, 1, sublift::PixelFormat::RGB24, 150);

    uint64_t h_wide = sublift::features::compute_dhash(img_wide_gray.view());
    CHECK(h_wide == 0ULL);

    auto mask_wide = sublift::features::compute_binary_mask(img_wide_gray.view());
    CHECK(mask_wide.width() == 1000);
    CHECK(mask_wide.height() == 1);

    double fg = sublift::features::compute_foreground_ratio(mask_wide.view());
    CHECK(fg >= 0.0);
    CHECK(fg <= 1.0);

    double ssim_wide = sublift::features::compute_ssim(img_wide_gray.view(), img_wide_gray.view());
    CHECK(ssim_wide == 1.0);
  }

  SECTION("Small images around Gaussian window size (2x2, 3x3, 5x5, 7x7)") {
    for (int sz : {2, 3, 5, 7}) {
      auto a = make_pattern_image(sz, sz, sublift::PixelFormat::Gray8, sz * 11);
      auto b = make_pattern_image(sz, sz, sublift::PixelFormat::Gray8, sz * 17);

      // Identity check
      CHECK(sublift::features::compute_ssim(a.view(), a.view(), 7) == 1.0);

      // Compute on differing small images
      double ssim_val = sublift::features::compute_ssim(a.view(), b.view(), 7);
      CHECK(ssim_val >= -1.0);
      CHECK(ssim_val <= 1.0);

      auto mask = sublift::features::compute_binary_mask(a.view());
      CHECK(mask.width() == sz);
      CHECK(mask.height() == sz);

      double mask_ssim = sublift::features::compute_mask_ssim(mask.view(), mask.view(), 7);
      CHECK(mask_ssim == 1.0);
    }
  }

  SECTION("Corrupt / Unusual Stride: Padded rows in ImageView") {
    // Construct a buffer with width=16, height=16, but stride=64 bytes
    const int w = 16;
    const int h = 16;
    const int stride = 64;
    std::vector<uint8_t> raw_data(stride * h, 0xAA);
    // Fill active area with known pattern
    for (int y = 0; y < h; ++y) {
      for (int x = 0; x < w; ++x) {
        raw_data[y * stride + x] = static_cast<uint8_t>((x + y) * 8);
      }
    }

    sublift::ImageView view(raw_data.data(), w, h, stride, sublift::PixelFormat::Gray8);

    uint64_t hash = sublift::features::compute_dhash(view, 8);
    CHECK(hash != 0ULL);

    auto mask = sublift::features::compute_binary_mask(view);
    CHECK(mask.width() == w);
    CHECK(mask.height() == h);

    double ssim_self = sublift::features::compute_ssim(view, view);
    CHECK(ssim_self == 1.0);
  }

  SECTION("Corrupt MaskConfig parameters do not crash") {
    auto img = make_uniform_image(64, 64, sublift::PixelFormat::Gray8, 128);

    // Negative block size ratio
    sublift::features::MaskConfig cfg1{.block_size_ratio = -0.5};
    auto m1 = sublift::features::compute_binary_mask(img.view(), cfg1);
    CHECK_FALSE(m1.empty());

    // Very large block size ratio
    sublift::features::MaskConfig cfg2{.block_size_ratio = 5.0};
    auto m2 = sublift::features::compute_binary_mask(img.view(), cfg2);
    CHECK_FALSE(m2.empty());

    // Zero / negative morph kernel
    sublift::features::MaskConfig cfg3{.morph_kernel_width = 0, .morph_kernel_height = -2};
    auto m3 = sublift::features::compute_binary_mask(img.view(), cfg3);
    CHECK_FALSE(m3.empty());
  }

  SECTION("Moved-from SsimWorkspace does not crash or corrupt memory") {
    sublift::features::SsimWorkspace ws1;
    ws1.ensure_capacity(64, 64);
    sublift::features::SsimWorkspace ws2 = std::move(ws1);

    // ws1 is now moved-from. Calling methods on it should safely fallback or no-op
    CHECK(ws1.raw_buffer_data() == nullptr);
    CHECK(ws1.capacity_elements() == 0);
    CHECK(ws1.current_rows() == 0);
    CHECK(ws1.current_cols() == 0);

    // ws1.compute should gracefully fall back to local mats without crashing
    auto img1 = make_uniform_image(32, 32, sublift::PixelFormat::Gray8, 100);
    auto img2 = make_uniform_image(32, 32, sublift::PixelFormat::Gray8, 200);
    double ssim_val = ws1.compute(img1.view(), img2.view(), 7);
    CHECK(ssim_val >= 0.0);
    CHECK(ssim_val <= 1.0);
  }

  SECTION("High aspect ratio non-uniform images (4096x16 and 16x4096)") {
    auto wide_img = make_pattern_image(4096, 16, sublift::PixelFormat::Gray8, 4096);
    auto tall_img = make_pattern_image(16, 4096, sublift::PixelFormat::Gray8, 4096);

    uint64_t wide_hash = sublift::features::compute_dhash(wide_img.view(), 8);
    CHECK(wide_hash != 0ULL);

    uint64_t tall_hash = sublift::features::compute_dhash(tall_img.view(), 8);
    CHECK(tall_hash != 0ULL);

    auto wide_mask = sublift::features::compute_binary_mask(wide_img.view());
    CHECK(wide_mask.width() == 4096);
    CHECK(wide_mask.height() == 16);

    auto tall_mask = sublift::features::compute_binary_mask(tall_img.view());
    CHECK(tall_mask.width() == 16);
    CHECK(tall_mask.height() == 4096);
  }

  SECTION("Text region geometry boundary stress") {
    // Normal case
    CHECK(sublift::features::is_valid_text_region(sublift::FrameLocalBox{100, 100, 300, 30}, 1920, 1080));

    // Right on frame edge (x + w == frame_w, y + h == frame_h)
    CHECK(sublift::features::is_valid_text_region(sublift::FrameLocalBox{1620, 1050, 300, 30}, 1920, 1080));

    // Exceeding frame boundary by 1 pixel
    CHECK_FALSE(sublift::features::is_valid_text_region(sublift::FrameLocalBox{1621, 1050, 300, 30}, 1920, 1080));
    CHECK_FALSE(sublift::features::is_valid_text_region(sublift::FrameLocalBox{1620, 1051, 300, 30}, 1920, 1080));

    // Zero-width or zero-height
    CHECK_FALSE(sublift::features::is_valid_text_region(sublift::FrameLocalBox{100, 100, 0, 30}, 1920, 1080));
    CHECK_FALSE(sublift::features::is_valid_text_region(sublift::FrameLocalBox{100, 100, 300, 0}, 1920, 1080));

    // Out-of-bounds coordinates near INT32_MAX must return false without signed overflow UB
    CHECK_FALSE(sublift::features::is_valid_text_region(sublift::FrameLocalBox{2000000000, 100, 1000000000, 30}, 1920, 1080));
    CHECK_FALSE(sublift::features::is_valid_text_region(sublift::FrameLocalBox{100, 2000000000, 300, 1000000000}, 1920, 1080));

    // Large frame dimensions (50000x50000) with a valid 10000x1000 box (aspect 10.0, area ratio 0.004 >= 0.001)
    // Must return true without signed integer overflow in frame_width * frame_height
    CHECK(sublift::features::is_valid_text_region(sublift::FrameLocalBox{100, 100, 10000, 1000}, 50000, 50000));
  }
}




