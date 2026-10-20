#include <catch2/catch_test_macros.hpp>

#include "sublift/image.hpp"

using sublift::bytes_per_pixel;
using sublift::ImageBuffer;
using sublift::ImageView;
using sublift::PixelFormat;

TEST_CASE("bytes_per_pixel", "[image]") {
  REQUIRE(bytes_per_pixel(PixelFormat::RGB24) == 3);
  REQUIRE(bytes_per_pixel(PixelFormat::BGR24) == 3);
  REQUIRE(bytes_per_pixel(PixelFormat::Gray8) == 1);
}

TEST_CASE("ImageBuffer allocate RGB24", "[image]") {
  ImageBuffer buf(4, 2, PixelFormat::RGB24);
  REQUIRE(buf.width() == 4);
  REQUIRE(buf.height() == 2);
  REQUIRE(buf.format() == PixelFormat::RGB24);
  REQUIRE(buf.stride_bytes() >= 12);
  REQUIRE_FALSE(buf.empty());
  REQUIRE(buf.data() != nullptr);
}

TEST_CASE("ImageBuffer tight Gray8", "[image]") {
  ImageBuffer buf(5, 3, PixelFormat::Gray8);
  REQUIRE(buf.stride_bytes() == 5);
}

TEST_CASE("ImageBuffer padded stride", "[image]") {
  ImageBuffer buf(2, 2, PixelFormat::Gray8, 8);
  REQUIRE(buf.stride_bytes() == 8);
  REQUIRE(buf.view().stride_bytes() == 8);
}

TEST_CASE("view shares storage", "[image]") {
  ImageBuffer buf(2, 2, PixelFormat::Gray8);
  auto view = buf.view();
  REQUIRE(view.data() == buf.data());
}

TEST_CASE("ROI shares storage without copy", "[image]") {
  ImageBuffer buf(4, 3, PixelFormat::Gray8);
  // Unique pattern: value = y*10 + x
  for (std::int32_t y = 0; y < 3; ++y) {
    for (std::int32_t x = 0; x < 4; ++x) {
      buf.data()[static_cast<std::size_t>(y) * static_cast<std::size_t>(buf.stride_bytes()) +
                 static_cast<std::size_t>(x)] =
          static_cast<std::uint8_t>(y * 10 + x);
    }
  }

  const ImageView roi = buf.roi(1, 1, 2, 1);
  const auto expected_ptr =
      buf.data() + 1 * buf.stride_bytes() + 1 * bytes_per_pixel(PixelFormat::Gray8);
  REQUIRE(roi.data() == expected_ptr);
  REQUIRE(roi.width() == 2);
  REQUIRE(roi.height() == 1);
  REQUIRE(roi.data()[0] == 11);
  REQUIRE(roi.data()[1] == 12);

  // Mutate parent; ROI must observe it (shared memory).
  buf.data()[static_cast<std::size_t>(1 * buf.stride_bytes() + 1)] = 99;
  REQUIRE(roi.data()[0] == 99);
}

TEST_CASE("nested ROI offsets", "[image]") {
  ImageBuffer buf(8, 8, PixelFormat::Gray8);
  for (std::int32_t i = 0; i < 64; ++i) {
    buf.data()[static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(i);
  }
  const ImageView outer = buf.roi(2, 2, 4, 4);
  const ImageView inner = outer.roi(1, 1, 2, 2);
  // Inner origin at (3,3) in parent
  REQUIRE(inner.data() == buf.data() + 3 * 8 + 3);
}

TEST_CASE("clone is independent", "[image]") {
  ImageBuffer buf(2, 1, PixelFormat::Gray8);
  buf.data()[0] = 7;
  buf.data()[1] = 8;
  ImageBuffer copy = buf.clone();
  REQUIRE(copy.data() != buf.data());
  buf.data()[0] = 1;
  REQUIRE(copy.data()[0] == 7);
}

TEST_CASE("invalid ROI throws", "[image]") {
  ImageBuffer buf(4, 4, PixelFormat::Gray8);
  REQUIRE_THROWS_AS(buf.roi(3, 0, 2, 1), std::invalid_argument);
  REQUIRE_THROWS_AS(buf.roi(-1, 0, 1, 1), std::invalid_argument);
}

TEST_CASE("move buffer", "[image]") {
  ImageBuffer buf(3, 1, PixelFormat::Gray8);
  buf.data()[0] = 5;
  ImageBuffer moved = std::move(buf);
  REQUIRE(moved.width() == 3);
  REQUIRE(moved.data()[0] == 5);
  REQUIRE(buf.empty());  // NOLINT(bugprone-use-after-move)
}

TEST_CASE("zero size empty", "[image]") {
  ImageBuffer buf(0, 0, PixelFormat::Gray8);
  REQUIRE(buf.empty());
  REQUIRE(buf.view().empty());
}

TEST_CASE("ImageBuffer rejects width*bpp overflow", "[image]") {
  // 1e9 * 3 overflows int32; must not accept undersized stride.
  REQUIRE_THROWS_AS(ImageBuffer(1'000'000'000, 1, PixelFormat::RGB24, 10),
                    std::invalid_argument);
  REQUIRE_THROWS_AS(ImageBuffer(1'000'000'000, 1, PixelFormat::RGB24, 0),
                    std::invalid_argument);
}

// Phase 16 D02 (O3): the default constructor keeps zero-filling (historical
// contract); the uninitialized factory is for full-overwrite call sites.
TEST_CASE("default ImageBuffer is zero-filled; factory is not", "[image]") {
  const ImageBuffer defaulted(4, 2, PixelFormat::RGB24);
  const std::size_t nbytes =
      static_cast<std::size_t>(defaulted.stride_bytes()) * 2;
  for (std::size_t i = 0; i < nbytes; ++i) {
    REQUIRE(defaulted.data()[i] == 0);
  }

  ImageBuffer uninit = ImageBuffer::allocate_uninitialized(
      4, 2, PixelFormat::RGB24);
  REQUIRE(uninit.width() == 4);
  REQUIRE(uninit.height() == 2);
  REQUIRE(uninit.stride_bytes() == defaulted.stride_bytes());
  REQUIRE_FALSE(uninit.empty());
  REQUIRE(uninit.data() != nullptr);

  // Probe (ASan/UBSan): write every byte, then read it back through a view.
  for (std::size_t i = 0; i < nbytes; ++i) {
    uninit.data()[i] = static_cast<std::uint8_t>(i * 7);
  }
  const ImageView view = uninit.view();
  for (std::size_t i = 0; i < nbytes; ++i) {
    REQUIRE(view.data()[i] == static_cast<std::uint8_t>(i * 7));
  }
}

TEST_CASE("allocate_uninitialized validates like the default path", "[image]") {
  REQUIRE_THROWS_AS(ImageBuffer::allocate_uninitialized(-1, 2, PixelFormat::Gray8),
                    std::invalid_argument);
  REQUIRE_THROWS_AS(ImageBuffer::allocate_uninitialized(2, 2, PixelFormat::Gray8, 1),
                    std::invalid_argument);
  const ImageBuffer empty = ImageBuffer::allocate_uninitialized(0, 0, PixelFormat::Gray8);
  REQUIRE(empty.empty());
}

TEST_CASE("clone of uninitialized-then-filled buffer matches source", "[image]") {
  ImageBuffer src(3, 2, PixelFormat::RGB24);
  for (std::size_t i = 0; i < 18; ++i) {
    src.data()[i] = static_cast<std::uint8_t>(i + 1);
  }
  const ImageBuffer copy = src.clone();
  REQUIRE(copy.stride_bytes() == src.stride_bytes());
  for (std::size_t i = 0; i < 18; ++i) {
    REQUIRE(copy.data()[i] == src.data()[i]);
  }
}
