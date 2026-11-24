#include "image_io.hpp"
#include "worker_app.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <streambuf>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <sublift/adapters/mock/mock_frame_source.hpp>
#include <sublift/adapters/mock/mock_ocr.hpp>
#include <sublift/core/image_buffer.hpp>

namespace {

using nlohmann::json;

struct TempDirGuard {
  std::filesystem::path path;
  explicit TempDirGuard(const std::string& name) {
    path = std::filesystem::temp_directory_path() /
           ("sublift_worker_unit_" + name + "_" +
            std::to_string(reinterpret_cast<std::uintptr_t>(this)));
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
    std::filesystem::create_directories(path, ec);
  }
  ~TempDirGuard() {
    std::error_code ec;
    std::filesystem::remove_all(path, ec);
  }
};

class PipeStreamBuf final : public std::streambuf {
 public:
  void push_line(const std::string& s) {
    {
      std::lock_guard<std::mutex> lock(mu_);
      for (char c : s) {
        buf_.push_back(c);
      }
    }
    cv_.notify_one();
  }

  void close_write() {
    {
      std::lock_guard<std::mutex> lock(mu_);
      closed_ = true;
    }
    cv_.notify_all();
  }

 protected:
  int_type underflow() override {
    if (gptr() < egptr()) {
      return traits_type::to_int_type(*gptr());
    }
    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait(lock, [this]() { return !buf_.empty() || closed_; });
    if (buf_.empty()) {
      return traits_type::eof();
    }
    ch_ = buf_.front();
    buf_.pop_front();
    setg(&ch_, &ch_, &ch_ + 1);
    return traits_type::to_int_type(ch_);
  }

 private:
  std::mutex mu_;
  std::condition_variable cv_;
  std::deque<char> buf_;
  bool closed_{false};
  char ch_{0};
};

class RecordingOutputStreamBuf final : public std::streambuf {
 public:
  void on_line(std::function<void(const std::string&)> cb) {
    std::lock_guard<std::mutex> lock(mu_);
    cb_ = std::move(cb);
  }

  [[nodiscard]] std::vector<json> parsed_lines() {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<json> out;
    for (const auto& l : lines_) {
      out.push_back(json::parse(l));
    }
    return out;
  }

 protected:
  int_type overflow(int_type ch) override {
    if (ch == traits_type::eof()) {
      return traits_type::not_eof(ch);
    }
    const char c = static_cast<char>(ch);
    if (c == '\n') {
      std::function<void(const std::string&)> cb_copy;
      std::string line_copy;
      {
        std::lock_guard<std::mutex> lock(mu_);
        line_copy = current_;
        lines_.push_back(current_);
        current_.clear();
        cb_copy = cb_;
      }
      if (cb_copy) {
        cb_copy(line_copy);
      }
    } else {
      std::lock_guard<std::mutex> lock(mu_);
      current_.push_back(c);
    }
    return ch;
  }

 private:
  std::mutex mu_;
  std::string current_;
  std::vector<std::string> lines_;
  std::function<void(const std::string&)> cb_;
};

sublift::ImageBuffer make_subtitle_frame(
    std::int32_t w, std::int32_t h, bool with_subtitle_bar) {
  sublift::ImageBuffer buf(w, h, sublift::PixelFormat::RGB24);
  std::memset(buf.data(), 16, static_cast<std::size_t>(h) * static_cast<std::size_t>(buf.stride_bytes()));
  if (with_subtitle_bar) {
    const std::int32_t y0 = std::max<std::int32_t>(0, h / 3);
    const std::int32_t y1 = std::min<std::int32_t>(h, (h * 2) / 3);
    const std::int32_t x0 = w / 6;
    const std::int32_t x1 = (w * 5) / 6;
    for (std::int32_t y = y0; y < y1; ++y) {
      std::uint8_t* row = buf.row(y);
      for (std::int32_t x = x0; x < x1; ++x) {
        const std::uint8_t val = ((x / 4) % 2 == 0) ? 245 : 30;
        row[x * 3 + 0] = val;
        row[x * 3 + 1] = val;
        row[x * 3 + 2] = val;
      }
    }
  }
  return buf;
}

class RecordingFrameSource final : public sublift::IFrameSource {
 public:
  sublift::VideoInfo info{.width = 640, .height = 360, .duration_ms = 5000, .fps = 25.0};
  std::vector<sublift::Frame> frames;
  std::vector<sublift::KeyframeThumbnail> thumbnails;
  sublift::ImageBuffer single_frame;
  std::optional<sublift::SourceBox>* recorded_crop{nullptr};
  std::atomic<bool> cancelled{false};

  sublift::VideoInfo probe(const std::filesystem::path&) override {
    return info;
  }

  sublift::FrameIOPlan plan(
      const sublift::VideoInfo& i,
      const std::optional<sublift::SourceBox>& requested_box) override {
    if (recorded_crop) {
      *recorded_crop = requested_box;
    }
    return sublift::FrameIOPlan{
        .output_crop = requested_box,
        .output_mode = requested_box.has_value()
                           ? sublift::FrameOutputMode::RoiRgb
                           : sublift::FrameOutputMode::FullRgb,
        .source = sublift::SourceFrameInfo{.width = i.width, .height = i.height},
    };
  }

  void extract(
      const std::filesystem::path&,
      double,
      const std::optional<sublift::SourceBox>& crop,
      std::function<bool(sublift::Frame)> consumer) override {
    if (recorded_crop) {
      *recorded_crop = crop;
    }
    for (const auto& f : frames) {
      if (cancelled.load(std::memory_order_acquire)) {
        break;
      }
      if (!consumer(sublift::Frame{f.timestamp_ms, f.image.clone()})) {
        break;
      }
    }
  }

  sublift::ImageBuffer extract_frame(
      const std::filesystem::path&,
      std::int64_t,
      const std::optional<sublift::SourceBox>& crop) override {
    if (recorded_crop) {
      *recorded_crop = crop;
    }
    if (!single_frame.empty()) {
      return single_frame.clone();
    }
    return make_subtitle_frame(info.width, info.height, true);
  }

  std::vector<sublift::KeyframeThumbnail> sample_keyframe_thumbnails(
      const std::filesystem::path&,
      double,
      std::size_t) override {
    std::vector<sublift::KeyframeThumbnail> out;
    for (const auto& t : thumbnails) {
      out.push_back(t.clone());
    }
    return out;
  }

  void cancel() override { cancelled.store(true, std::memory_order_release); }
  bool is_cancelled() const noexcept override {
    return cancelled.load(std::memory_order_acquire);
  }
};

class BlockingFrameSource final : public sublift::IFrameSource {
 public:
  BlockingFrameSource(
      std::promise<void>& entered_extract,
      std::atomic<bool>& cancelled_flag)
      : entered_extract_(entered_extract),
        cancelled_flag_(cancelled_flag) {}

  sublift::VideoInfo probe(const std::filesystem::path&) override {
    return sublift::VideoInfo{.width = 320, .height = 180, .duration_ms = 2000, .fps = 5.0};
  }

  sublift::FrameIOPlan plan(
      const sublift::VideoInfo& i,
      const std::optional<sublift::SourceBox>& requested_box) override {
    return sublift::FrameIOPlan{
        .output_crop = requested_box,
        .output_mode = requested_box.has_value()
                           ? sublift::FrameOutputMode::RoiRgb
                           : sublift::FrameOutputMode::FullRgb,
        .source = sublift::SourceFrameInfo{.width = i.width, .height = i.height},
    };
  }

  void extract(
      const std::filesystem::path&,
      double,
      const std::optional<sublift::SourceBox>&,
      std::function<bool(sublift::Frame)> consumer) override {
    consumer(sublift::Frame{0, make_subtitle_frame(320, 120, false)});
    if (!signaled_.exchange(true)) {
      entered_extract_.set_value();
    }
    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait(lock, [this]() { return cancelled_.load(std::memory_order_acquire); });
  }

  sublift::ImageBuffer extract_frame(
      const std::filesystem::path&,
      std::int64_t,
      const std::optional<sublift::SourceBox>&) override {
    return sublift::ImageBuffer{};
  }

  void cancel() override {
    {
      std::lock_guard<std::mutex> lock(mu_);
      cancelled_.store(true, std::memory_order_release);
      cancelled_flag_.store(true, std::memory_order_release);
    }
    cv_.notify_all();
  }

  bool is_cancelled() const noexcept override {
    return cancelled_.load(std::memory_order_acquire);
  }

 private:
  std::promise<void>& entered_extract_;
  std::atomic<bool>& cancelled_flag_;
  std::atomic<bool> signaled_{false};
  std::atomic<bool> cancelled_{false};
  std::mutex mu_;
  std::condition_variable cv_;
};

class MidOcrBlockingFrameSource final : public sublift::IFrameSource {
 public:
  explicit MidOcrBlockingFrameSource(std::atomic<bool>& cancelled_flag)
      : cancelled_flag_(cancelled_flag) {}

  sublift::VideoInfo probe(const std::filesystem::path&) override {
    return sublift::VideoInfo{
        .width = 640,
        .height = 360,
        .duration_ms = 3000,
        .fps = 25.0,
        .display_transform_ok = true,
    };
  }

  sublift::FrameIOPlan plan(
      const sublift::VideoInfo& i,
      const std::optional<sublift::SourceBox>& requested_box) override {
    return sublift::FrameIOPlan{
        .output_crop = requested_box,
        .output_mode = requested_box.has_value()
                           ? sublift::FrameOutputMode::RoiRgb
                           : sublift::FrameOutputMode::FullRgb,
        .source = sublift::SourceFrameInfo{.width = i.width, .height = i.height},
    };
  }

  void extract(
      const std::filesystem::path&,
      double,
      const std::optional<sublift::SourceBox>&,
      std::function<bool(sublift::Frame)> consumer) override {
    for (int i = 0; i < 15; ++i) {
      if (cancelled_.load(std::memory_order_acquire)) {
        return;
      }
      const bool has_sub = (i >= 3 && i <= 10);
      if (!consumer(sublift::Frame{
              .timestamp_ms = i * 200,
              .image = make_subtitle_frame(640, 180, has_sub),
          })) {
        return;
      }
    }
    std::unique_lock<std::mutex> lock(mu_);
    cv_.wait(lock, [this]() { return cancelled_.load(std::memory_order_acquire); });
  }

  sublift::ImageBuffer extract_frame(
      const std::filesystem::path&,
      std::int64_t,
      const std::optional<sublift::SourceBox>&) override {
    return sublift::ImageBuffer{};
  }

  void cancel() override {
    {
      std::lock_guard<std::mutex> lock(mu_);
      cancelled_.store(true, std::memory_order_release);
      cancelled_flag_.store(true, std::memory_order_release);
    }
    cv_.notify_all();
  }

  bool is_cancelled() const noexcept override {
    return cancelled_.load(std::memory_order_acquire);
  }

 private:
  std::atomic<bool>& cancelled_flag_;
  std::atomic<bool> cancelled_{false};
  std::mutex mu_;
  std::condition_variable cv_;
};

class BlockingOcrEngine final : public sublift::IOcrEngine {
 public:
  BlockingOcrEngine(
      std::promise<void>& entered_ocr,
      std::shared_future<void> release_ocr)
      : entered_ocr_(entered_ocr), release_ocr_(std::move(release_ocr)) {}

  sublift::OcrResult recognize(const sublift::ImageView& image) override {
    if (!signaled_.exchange(true)) {
      entered_ocr_.set_value();
    }
    release_ocr_.wait();
    return sublift::OcrResult{
        .text = "Late OCR Line",
        .confidence = 0.95,
        .lines = {sublift::OcrLine{
            .text = "Late OCR Line",
            .confidence = 0.95,
            .box = sublift::OcrCropBox{0, 0, image.width(), image.height()},
        }},
    };
  }

  std::string engine_name() const override { return "blocking_mock"; }

 private:
  std::promise<void>& entered_ocr_;
  std::shared_future<void> release_ocr_;
  std::atomic<bool> signaled_{false};
};

class MockWorkerBackend final : public sublift::worker::IWorkerBackend {
 public:
  sublift::worker::CapabilitiesInfo caps{
      .engines = {"paddle"},
      .ffmpeg_available = true,
      .ffprobe_available = true,
      .hwaccel_modes = {"auto", "none"},
      .platform = "macos",
  };
  std::function<std::unique_ptr<sublift::IFrameSource>(double, std::int32_t)> source_factory;
  std::shared_ptr<sublift::adapters::mock::MockOcrEngine> mock_ocr =
      std::make_shared<sublift::adapters::mock::MockOcrEngine>("Mock Subtitle Line", 0.94);
  std::shared_ptr<sublift::IOcrEngine> ocr_override;
  std::optional<sublift::PixelFormat> last_ocr_input_format;
  bool allow_paddle{true};
  bool allow_apple_vision{false};

  sublift::worker::CapabilitiesInfo query_capabilities() override { return caps; }

  std::unique_ptr<sublift::IFrameSource> create_frame_source(
      double fps,
      const std::optional<std::string>&,
      std::int32_t max_output_width) override {
    if (source_factory) {
      return source_factory(fps, max_output_width);
    }
    return std::make_unique<RecordingFrameSource>();
  }

  sublift::worker::OcrAcquireResult acquire_ocr_engine(
      const std::string& engine_id,
      std::int32_t) override {
    class FormatSpyOcr final : public sublift::IOcrEngine {
     public:
      FormatSpyOcr(std::shared_ptr<sublift::IOcrEngine> inner,
                   std::optional<sublift::PixelFormat>* fmt_out)
          : inner_(std::move(inner)), fmt_out_(fmt_out) {}
      sublift::OcrResult recognize(const sublift::ImageView& image) override {
        if (fmt_out_) {
          *fmt_out_ = image.format();
        }
        return inner_->recognize(image);
      }
      std::string engine_name() const override { return inner_->engine_name(); }

     private:
      std::shared_ptr<sublift::IOcrEngine> inner_;
      std::optional<sublift::PixelFormat>* fmt_out_;
    };

    const auto& active_ocr = ocr_override
        ? ocr_override
        : std::static_pointer_cast<sublift::IOcrEngine>(mock_ocr);

    if (engine_id == "paddle") {
      if (!allow_paddle) {
        return {nullptr, "EngineUnavailable", "Paddle unavailable in mock"};
      }
      return {std::make_shared<FormatSpyOcr>(active_ocr, &last_ocr_input_format), "", ""};
    }
    if (engine_id == "apple_vision") {
      if (!allow_apple_vision) {
        return {nullptr, "EngineUnavailable", "Apple Vision unavailable in mock"};
      }
      return {std::make_shared<FormatSpyOcr>(active_ocr, &last_ocr_input_format), "", ""};
    }
    return {nullptr, "InvalidConfiguration", "Unknown engine: " + engine_id};
  }
};

std::uint32_t read_be32(const std::uint8_t* p) {
  return (static_cast<std::uint32_t>(p[0]) << 24U) |
         (static_cast<std::uint32_t>(p[1]) << 16U) |
         (static_cast<std::uint32_t>(p[2]) << 8U) |
         static_cast<std::uint32_t>(p[3]);
}

}  // namespace

TEST_CASE("Worker image_io: PNG stored-deflate, PPM roundtrip, downscaler, and atomic write",
          "[worker][unit]") {
  TempDirGuard tmp("image_io");

  SECTION("write_png_atomic emits valid RGB24 PNG with IHDR/IDAT/IEND and no leftover temp files") {
    sublift::ImageBuffer img(24, 12, sublift::PixelFormat::RGB24);
    for (int y = 0; y < 12; ++y) {
      std::uint8_t* row = img.row(y);
      for (int x = 0; x < 24; ++x) {
        row[x * 3 + 0] = static_cast<std::uint8_t>(x * 10);
        row[x * 3 + 1] = static_cast<std::uint8_t>(y * 20);
        row[x * 3 + 2] = 99;
      }
    }
    const auto png_path = tmp.path / "nested" / "frame.png";
    sublift::worker::write_png_atomic(png_path, img.view());
    REQUIRE(std::filesystem::exists(png_path));

    // Verify zero leftover .tmp_* files
    int file_count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(png_path.parent_path())) {
      ++file_count;
      REQUIRE(entry.path().filename().string() == "frame.png");
    }
    REQUIRE(file_count == 1);

    std::ifstream ifs(png_path, std::ios::binary);
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(ifs)),
                                    std::istreambuf_iterator<char>());
    // Magic (8) + IHDR chunk (25) + IDAT chunk (>= 23) + IEND chunk (12)
    REQUIRE(bytes.size() > 60);
    const std::vector<std::uint8_t> kSig{0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A};
    REQUIRE(std::equal(kSig.begin(), kSig.end(), bytes.begin()));

    // IHDR at offset 8
    REQUIRE(read_be32(&bytes[8]) == 13);
    REQUIRE(std::string(reinterpret_cast<const char*>(&bytes[12]), 4) == "IHDR");
    REQUIRE(read_be32(&bytes[16]) == 24);
    REQUIRE(read_be32(&bytes[20]) == 12);
    REQUIRE(bytes[24] == 8);  // bit depth
    REQUIRE(bytes[25] == 2);  // RGB color type

    // IDAT at offset 33
    const std::uint32_t idat_len = read_be32(&bytes[33]);
    REQUIRE(std::string(reinterpret_cast<const char*>(&bytes[37]), 4) == "IDAT");
    REQUIRE(bytes[41] == 0x78);
    REQUIRE(bytes[42] == 0x01);
    REQUIRE(idat_len == 2U + 5U + (12U * (1U + 24U * 3U)) + 4U);

    // IEND at end
    const std::size_t iend_off = bytes.size() - 12;
    REQUIRE(read_be32(&bytes[iend_off]) == 0);
    REQUIRE(std::string(reinterpret_cast<const char*>(&bytes[iend_off + 4]), 4) == "IEND");
  }

  SECTION("write_ppm_atomic + load_ppm + ensure_rgb24 roundtrip for P6 and P5") {
    sublift::ImageBuffer rgb(10, 6, sublift::PixelFormat::RGB24);
    rgb.row(0)[0] = 12;
    rgb.row(0)[1] = 34;
    rgb.row(0)[2] = 56;
    const auto p6_path = tmp.path / "test_p6.ppm";
    sublift::worker::write_ppm_atomic(p6_path, rgb.view());
    auto loaded_p6 = sublift::worker::load_ppm(p6_path);
    REQUIRE(loaded_p6.width() == 10);
    REQUIRE(loaded_p6.height() == 6);
    REQUIRE(loaded_p6.format() == sublift::PixelFormat::RGB24);
    REQUIRE(loaded_p6.row(0)[0] == 12);
    REQUIRE(loaded_p6.row(0)[1] == 34);
    REQUIRE(loaded_p6.row(0)[2] == 56);

    sublift::ImageBuffer gray(8, 4, sublift::PixelFormat::Gray8);
    gray.row(2)[3] = 200;
    const auto p5_path = tmp.path / "test_p5.ppm";
    sublift::worker::write_ppm_atomic(p5_path, gray.view());
    auto loaded_p5 = sublift::worker::load_ppm(p5_path);
    REQUIRE(loaded_p5.format() == sublift::PixelFormat::Gray8);
    REQUIRE(loaded_p5.row(2)[3] == 200);

    auto expanded = sublift::worker::ensure_rgb24(loaded_p5.view());
    REQUIRE(expanded.format() == sublift::PixelFormat::RGB24);
    REQUIRE(expanded.row(2)[3 * 3 + 0] == 200);
    REQUIRE(expanded.row(2)[3 * 3 + 1] == 200);
    REQUIRE(expanded.row(2)[3 * 3 + 2] == 200);
  }

  SECTION("resize_max_width preserves aspect ratio and respects max_width <= 0 or larger") {
    sublift::ImageBuffer src(400, 200, sublift::PixelFormat::RGB24);
    auto scaled = sublift::worker::resize_max_width(src.view(), 100);
    REQUIRE(scaled.width() == 100);
    REQUIRE(scaled.height() == 50);

    auto unchanged = sublift::worker::resize_max_width(src.view(), 500);
    REQUIRE(unchanged.width() == 400);
    REQUIRE(unchanged.height() == 200);
  }

  SECTION("write_png_atomic handles multi-block stored deflate (>65535 raw bytes) and Gray8") {
    // 200x120 RGB24 => 120 * (1 + 600) = 72,120 raw bytes => 2 stored deflate blocks
    sublift::ImageBuffer big(200, 120, sublift::PixelFormat::RGB24);
    std::memset(big.data(), 0x5A, static_cast<std::size_t>(120) * static_cast<std::size_t>(big.stride_bytes()));
    const auto big_png = tmp.path / "big_rgb.png";
    sublift::worker::write_png_atomic(big_png, big.view());
    {
      std::ifstream ifs(big_png, std::ios::binary);
      std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(ifs)),
                                      std::istreambuf_iterator<char>());
      // Magic(8) + IHDR(25) + IDAT(8 + 2 + 5 + 65535 + 5 + (72120-65535) + 4 + 4) + IEND(12)
      const std::uint32_t expected_idat_len = 2U + 5U + 65535U + 5U + (72120U - 65535U) + 4U;
      REQUIRE(read_be32(&bytes[16]) == 200);
      REQUIRE(read_be32(&bytes[20]) == 120);
      REQUIRE(bytes[25] == 2);  // RGB24
      REQUIRE(read_be32(&bytes[33]) == expected_idat_len);
      // Block 1 header at offset 43: BFINAL=0, LEN=65535
      REQUIRE(bytes[43] == 0x00);
      REQUIRE(bytes[44] == 0xFF);
      REQUIRE(bytes[45] == 0xFF);
      // Block 2 header at offset 43 + 5 + 65535: BFINAL=1, LEN=6585
      const std::size_t b2 = 43U + 5U + 65535U;
      REQUIRE(bytes[b2] == 0x01);
      const std::uint16_t b2_len = static_cast<std::uint16_t>(bytes[b2 + 1]) |
                                   (static_cast<std::uint16_t>(bytes[b2 + 2]) << 8U);
      REQUIRE(b2_len == 72120U - 65535U);
    }

    sublift::ImageBuffer gray(16, 8, sublift::PixelFormat::Gray8);
    std::memset(gray.data(), 0x80, static_cast<std::size_t>(8) * static_cast<std::size_t>(gray.stride_bytes()));
    const auto gray_png = tmp.path / "gray.png";
    sublift::worker::write_png_atomic(gray_png, gray.view());
    {
      std::ifstream ifs(gray_png, std::ios::binary);
      std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(ifs)),
                                      std::istreambuf_iterator<char>());
      REQUIRE(read_be32(&bytes[16]) == 16);
      REQUIRE(read_be32(&bytes[20]) == 8);
      REQUIRE(bytes[25] == 0);  // Gray8
      REQUIRE(read_be32(&bytes[33]) == 2U + 5U + (8U * (1U + 16U)) + 4U);
    }
  }

  SECTION("load_ppm handles CRLF after maxval and rejects malformed headers/OOM sizes") {
    const auto write_raw = [](const std::filesystem::path& p, const std::string& data) {
      std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
      ofs.write(data.data(), static_cast<std::streamsize>(data.size()));
    };

    // (a) CRLF after maxval must NOT shift RGB channels
    const auto crlf_ppm = tmp.path / "crlf.ppm";
    write_raw(crlf_ppm, std::string("P6\n2 1\n255\r\n") +
                            std::string("\x0a\x14\x1e\x28\x32\x3c", 6));
    auto loaded_crlf = sublift::worker::load_ppm(crlf_ppm);
    REQUIRE(loaded_crlf.width() == 2);
    REQUIRE(loaded_crlf.height() == 1);
    REQUIRE(loaded_crlf.row(0)[0] == 10);
    REQUIRE(loaded_crlf.row(0)[1] == 20);
    REQUIRE(loaded_crlf.row(0)[2] == 30);
    REQUIRE(loaded_crlf.row(0)[3] == 40);
    REQUIRE(loaded_crlf.row(0)[4] == 50);
    REQUIRE(loaded_crlf.row(0)[5] == 60);

    // (a2) Single CR delimiter followed by pixel byte 0x0A must NOT be misparsed as CRLF
    const auto cr_pixel_0a_ppm = tmp.path / "cr_pixel_0a.ppm";
    write_raw(cr_pixel_0a_ppm, std::string("P6\n2 1\n255\r") +
                                   std::string("\x0a\x14\x1e\x28\x32\x3c", 6));
    auto loaded_cr_0a = sublift::worker::load_ppm(cr_pixel_0a_ppm);
    REQUIRE(loaded_cr_0a.width() == 2);
    REQUIRE(loaded_cr_0a.height() == 1);
    REQUIRE(loaded_cr_0a.row(0)[0] == 10);
    REQUIRE(loaded_cr_0a.row(0)[1] == 20);
    REQUIRE(loaded_cr_0a.row(0)[2] == 30);
    REQUIRE(loaded_cr_0a.row(0)[3] == 40);
    REQUIRE(loaded_cr_0a.row(0)[4] == 50);
    REQUIRE(loaded_cr_0a.row(0)[5] == 60);

    // (b) Comment immediately after maxval without whitespace must fail
    const auto comment_after_max = tmp.path / "comment_max.ppm";
    write_raw(comment_after_max, std::string("P6\n2 1\n255#c\n") +
                                     std::string("\x0a\x14\x1e\x28\x32\x3c", 6));
    REQUIRE_THROWS_AS(sublift::worker::load_ppm(comment_after_max), std::runtime_error);

    // (c) Trailing non-digits in integer header token must fail
    const auto partial_int = tmp.path / "partial_int.ppm";
    write_raw(partial_int, std::string("P6\n2abc 1\n255\n") +
                               std::string("\x0a\x14\x1e\x28\x32\x3c", 6));
    REQUIRE_THROWS_AS(sublift::worker::load_ppm(partial_int), std::runtime_error);

    // (d) Huge dimensions with short payload must fail BEFORE allocating 30 GB
    const auto oom_header = tmp.path / "oom_header.ppm";
    write_raw(oom_header, "P6\n100000 100000\n255\nshort");
    REQUIRE_THROWS_AS(sublift::worker::load_ppm(oom_header), std::runtime_error);

    // (e) Zero / >255 maxval must fail
    const auto zero_max = tmp.path / "zero_max.ppm";
    write_raw(zero_max, "P6\n2 1\n0\n\x01\x02\x03\x04\x05\x06");
    REQUIRE_THROWS_AS(sublift::worker::load_ppm(zero_max), std::runtime_error);

    const auto over_max = tmp.path / "over_max.ppm";
    write_raw(over_max, "P6\n2 1\n256\n\x01\x02\x03\x04\x05\x06");
    REQUIRE_THROWS_AS(sublift::worker::load_ppm(over_max), std::runtime_error);
  }
}

TEST_CASE("WorkerServer protocol: capabilities, finalize_srt, validation & fail-closed engines",
          "[worker][unit]") {
  MockWorkerBackend backend;
  sublift::worker::WorkerServer server(backend);

  PipeStreamBuf in_buf;
  RecordingOutputStreamBuf out_buf;
  std::mutex wait_mu;
  std::condition_variable wait_cv;
  std::vector<json> msgs;
  out_buf.on_line([&](const std::string& raw) {
    {
      std::lock_guard<std::mutex> lock(wait_mu);
      msgs.push_back(json::parse(raw));
    }
    wait_cv.notify_all();
  });

  std::istream in(&in_buf);
  std::ostream out(&out_buf);
  int exit_code = -1;
  std::thread server_thread([&]() { exit_code = server.run(in, out); });

  in_buf.push_line(
      "not json\n"
      "{\"id\":1,\"op\":\"capabilities\",\"params\":{}}\n"
      "{\"id\":2,\"op\":\"unknown_op\",\"params\":{}}\n"
      "{\"id\":3,\"op\":\"finalize_srt\",\"params\":{\"apply_merge\":false,\"entries\":["
      "{\"id\":1,\"start_ms\":1000,\"end_ms\":2000,\"text\":\"First\",\"confidence\":0.9},"
      "{\"id\":2,\"start_ms\":2050,\"end_ms\":3000,\"text\":\"First\",\"confidence\":0.9}"
      "]}}\n"
      "{\"id\":4,\"op\":\"finalize_srt\",\"params\":{\"apply_merge\":true,\"min_duration_ms\":0,\"merge_gap_ms\":1000,\"entries\":["
      "{\"id\":1,\"start_ms\":1000,\"end_ms\":2000,\"text\":\"First\",\"confidence\":0.9},"
      "{\"id\":2,\"start_ms\":2050,\"end_ms\":3000,\"text\":\"First\",\"confidence\":0.9}"
      "]}}\n"
      "{\"id\":5,\"op\":\"recognize_strips\",\"params\":{\"engine\":\"apple_vision\",\"strip_paths\":[],\"script\":\"auto\"}}\n");

  // recognize_strips (id: 5) runs asynchronously via dispatch_async while finalize_srt (id: 6..9)
  // runs synchronously on the stdin reader loop. Wait deterministically for id: 5 before
  // pushing id: 6..9 so async/sync response ordering cannot race.
  {
    std::unique_lock<std::mutex> lock(wait_mu);
    wait_cv.wait(lock, [&]() {
      if (msgs.size() < 6) {
        return false;
      }
      for (const auto& m : msgs) {
        if (m.contains("id") && m["id"] == 5 && m.value("type", "") == "result") {
          return true;
        }
      }
      return false;
    });
  }

  in_buf.push_line(
      "{\"id\":6,\"op\":\"finalize_srt\",\"params\":{\"apply_merge\":false,\"entries\":["
      "{\"id\":1,\"start_ms\":2000,\"end_ms\":1000,\"text\":\"Inverted\",\"confidence\":0.9}"
      "]}}\n"
      "{\"id\":7,\"op\":\"finalize_srt\",\"params\":{\"apply_merge\":false,\"entries\":["
      "{\"id\":1,\"start_ms\":1000,\"end_ms\":1000,\"text\":\"ZeroDuration\",\"confidence\":0.9}"
      "]}}\n"
      "{\"id\":8,\"op\":\"finalize_srt\",\"params\":{\"apply_merge\":false,\"entries\":["
      "{\"id\":1,\"start_ms\":-200,\"end_ms\":-100,\"text\":\"NegativeEnd\",\"confidence\":0.9}"
      "]}}\n"
      "{\"id\":9,\"op\":\"finalize_srt\",\"params\":{\"apply_merge\":false,\"entries\":["
      "{\"id\":1,\"start_ms\":-200,\"end_ms\":1200,\"text\":\"NegativeStartPositiveEnd\",\"confidence\":0.9}"
      "]}}\n");

  {
    std::unique_lock<std::mutex> lock(wait_mu);
    wait_cv.wait(lock, [&]() { return msgs.size() >= 10; });
  }
  in_buf.close_write();
  server_thread.join();
  REQUIRE(exit_code == 0);
  REQUIRE(msgs.size() == 10);

  const auto find_msg_by_id = [&](std::uint64_t target_id) -> const json& {
    const json* match = nullptr;
    for (const auto& m : msgs) {
      if (m.contains("id") && m["id"].is_number_unsigned() &&
          m["id"].get<std::uint64_t>() == target_id) {
        REQUIRE(match == nullptr);
        match = &m;
      }
    }
    REQUIRE(match != nullptr);
    return *match;
  };

  const auto& msg0 = find_msg_by_id(0);
  REQUIRE(msg0["ok"] == false);
  REQUIRE(msg0["error"]["code"] == "InvalidRequest");

  const auto& msg1 = find_msg_by_id(1);
  REQUIRE(msg1["ok"] == true);
  REQUIRE(msg1["data"]["engines"] == json::array({"paddle"}));
  REQUIRE(msg1["data"]["ffmpeg_available"] == true);
  REQUIRE(msg1["data"]["ffprobe_available"] == true);
  REQUIRE(msg1["data"]["platform"] == "macos");

  const auto& msg2 = find_msg_by_id(2);
  REQUIRE(msg2["ok"] == false);
  REQUIRE(msg2["error"]["code"] == "InvalidRequest");

  // apply_merge=false keeps 2 separate cues
  const auto& msg3 = find_msg_by_id(3);
  REQUIRE(msg3["ok"] == true);
  REQUIRE(msg3["data"]["cue_count"] == 2);
  REQUIRE(msg3["data"]["srt_text"].get<std::string>().find("2\n00:00:02,050 --> 00:00:03,000") !=
          std::string::npos);

  // apply_merge=true merges identical adjacent cues into 1
  const auto& msg4 = find_msg_by_id(4);
  REQUIRE(msg4["ok"] == true);
  REQUIRE(msg4["data"]["cue_count"] == 1);
  REQUIRE(msg4["data"]["srt_text"].get<std::string>().find("00:00:01,000 --> 00:00:03,000") !=
          std::string::npos);

  // fail-closed EngineUnavailable when apple_vision is disabled in mock
  const auto& msg5 = find_msg_by_id(5);
  REQUIRE(msg5["ok"] == false);
  REQUIRE(msg5["error"]["code"] == "EngineUnavailable");

  // finalize_srt rejects end_ms <= start_ms and end_ms <= 0 with InvalidRequest
  const auto& msg6 = find_msg_by_id(6);
  REQUIRE(msg6["ok"] == false);
  REQUIRE(msg6["error"]["code"] == "InvalidRequest");

  const auto& msg7 = find_msg_by_id(7);
  REQUIRE(msg7["ok"] == false);
  REQUIRE(msg7["error"]["code"] == "InvalidRequest");

  const auto& msg8 = find_msg_by_id(8);
  REQUIRE(msg8["ok"] == false);
  REQUIRE(msg8["error"]["code"] == "InvalidRequest");

  // start_ms < 0 with end_ms > 0 remains allowed and clamps start to 00:00:00,000
  const auto& msg9 = find_msg_by_id(9);
  REQUIRE(msg9["ok"] == true);
  REQUIRE(msg9["data"]["cue_count"] == 1);
  REQUIRE(msg9["data"]["srt_text"].get<std::string>().find("00:00:00,000 --> 00:00:01,200") !=
          std::string::npos);
}

TEST_CASE("WorkerServer ops: probe_video, extract_frame, detect_auto_region, dual-mode extraction, recognize_strips",
          "[worker][unit]") {
  TempDirGuard tmp("ops");
  MockWorkerBackend backend;

  std::optional<sublift::SourceBox> last_crop;
  std::vector<sublift::Frame> mock_frames;
  // Build deterministic 3-second sequence @ 5fps: blank -> subtitle -> blank
  for (int i = 0; i < 15; ++i) {
    const bool has_sub = (i >= 3 && i <= 10);
    mock_frames.push_back(sublift::Frame{
        .timestamp_ms = i * 200,
        .image = make_subtitle_frame(640, 180, has_sub),
    });
  }

  backend.source_factory = [&](double, std::int32_t) {
    auto src = std::make_unique<RecordingFrameSource>();
    src->info = sublift::VideoInfo{
        .width = 640,
        .height = 360,
        .duration_ms = 3000,
        .fps = 25.0,
        .display_transform_ok = true,
    };
    src->recorded_crop = &last_crop;
    for (const auto& f : mock_frames) {
      src->frames.push_back(sublift::Frame{f.timestamp_ms, f.image.clone()});
    }
    return src;
  };

  // Write a P5 Gray8 PPM strip to verify Gray8 -> RGB24 normalization in recognize_strips
  const auto gray_strip = tmp.path / "gray_strip.ppm";
  sublift::ImageBuffer gray_buf(120, 40, sublift::PixelFormat::Gray8);
  std::memset(gray_buf.data(), 200, 120U * 40U);
  sublift::worker::write_ppm_atomic(gray_strip, gray_buf.view());

  const auto frame_png = tmp.path / "preview.png";
  const auto strips_dir = tmp.path / "strips";
  const auto missing_strip = tmp.path / "does_not_exist.ppm";

  PipeStreamBuf in_buf;
  RecordingOutputStreamBuf out_buf;
  std::mutex wait_mu;
  std::condition_variable wait_cv;
  std::unordered_map<std::uint64_t, json> results;
  std::vector<json> local_events;
  std::vector<json> pure_events;

  out_buf.on_line([&](const std::string& raw) {
    json j = json::parse(raw);
    const std::uint64_t id = j["id"].get<std::uint64_t>();
    {
      std::lock_guard<std::mutex> lock(wait_mu);
      if (j["type"] == "result") {
        results[id] = j;
      } else if (j["type"] == "event") {
        if (id == 13) local_events.push_back(j);
        if (id == 14) pure_events.push_back(j);
      }
    }
    wait_cv.notify_all();
  });

  std::istream in(&in_buf);
  std::ostream out(&out_buf);
  sublift::worker::WorkerServer server(backend);
  int exit_code = -1;
  std::thread server_thread([&]() { exit_code = server.run(in, out); });

  const auto send_and_wait = [&](const json& req) {
    const std::uint64_t req_id = req["id"].get<std::uint64_t>();
    in_buf.push_line(req.dump() + "\n");
    std::unique_lock<std::mutex> lock(wait_mu);
    wait_cv.wait(lock, [&]() { return results.find(req_id) != results.end(); });
  };

  send_and_wait({{"id", 10}, {"op", "probe_video"}, {"params", {{"video_path", "dummy.mp4"}}}});
  send_and_wait({{"id", 11},
                 {"op", "extract_frame"},
                 {"params",
                  {{"video_path", "dummy.mp4"},
                   {"timestamp_ms", 400},
                   {"max_width", 320},
                   {"out_path", frame_png.string()}}}});
  send_and_wait({{"id", 12},
                 {"op", "detect_auto_region"},
                 {"params",
                  {{"video_path", "dummy.mp4"},
                   {"engine", "paddle"},
                   {"script", "auto"},
                   {"bottom_ratio", 0.45}}}});
  send_and_wait({{"id", 13},
                 {"op", "run_extraction"},
                 {"params",
                  {{"video_path", "dummy.mp4"},
                   {"mode", "local_ocr"},
                   {"engine", "paddle"},
                   {"config",
                    {{"sample_fps", 5.0},
                     {"region_bottom_ratio", 0.25},
                     {"subtitle_script", "auto"}}}}}});
  REQUIRE(last_crop.has_value());
  REQUIRE(last_crop->width == 640);
  REQUIRE(last_crop->height == 90);
  REQUIRE(last_crop->y == 270);

  send_and_wait({{"id", 14},
                 {"op", "run_extraction"},
                 {"params",
                  {{"video_path", "dummy.mp4"},
                   {"mode", "pure_timing_strips"},
                   {"strips_dir", strips_dir.string()},
                   {"region_box", {{"x", 0}, {"y", 180}, {"width", 640}, {"height", 180}}},
                   {"config", {{"sample_fps", 5.0}, {"subtitle_script", "auto"}}}}}});
  send_and_wait({{"id", 15},
                 {"op", "recognize_strips"},
                 {"params",
                  {{"engine", "paddle"},
                   {"strip_paths", {gray_strip.string(), missing_strip.string()}},
                   {"script", "latin"}}}});

  in_buf.close_write();
  server_thread.join();
  REQUIRE(exit_code == 0);

  // probe_video
  REQUIRE(results[10]["ok"] == true);
  REQUIRE(results[10]["data"]["width"] == 640);
  REQUIRE(results[10]["data"]["height"] == 360);

  // extract_frame
  REQUIRE(results[11]["ok"] == true);
  REQUIRE(results[11]["data"]["width"] == 320);
  REQUIRE(results[11]["data"]["height"] == 180);
  REQUIRE(std::filesystem::exists(frame_png));

  // detect_auto_region
  REQUIRE(results[12]["ok"] == true);
  REQUIRE(results[12]["data"].contains("region_box"));
  REQUIRE(results[12]["data"].contains("detected"));

  // local_ocr
  REQUIRE(results[13]["ok"] == true);
  REQUIRE_FALSE(results[13]["data"]["entries"].empty());
  bool saw_local_entry = false;
  bool saw_progress = false;
  for (const auto& ev : local_events) {
    REQUIRE(ev["event"] != "timing_slot");
    if (ev["event"] == "subtitle_entry") saw_local_entry = true;
    if (ev["event"] == "progress") {
      saw_progress = true;
      const double ev_fps = ev["data"]["fps"].get<double>();
      REQUIRE(std::isfinite(ev_fps));
      REQUIRE(ev_fps > 0.0);
    }
  }
  REQUIRE(saw_local_entry);
  REQUIRE(saw_progress);

  // pure_timing_strips
  REQUIRE(results[14]["ok"] == true);
  REQUIRE_FALSE(results[14]["data"]["entries"].empty());
  bool saw_timing_slot = false;
  for (const auto& ev : pure_events) {
    REQUIRE(ev["event"] != "subtitle_entry");
    if (ev["event"] == "timing_slot") {
      saw_timing_slot = true;
      const std::string sp = ev["data"]["strip_path"].get<std::string>();
      REQUIRE(std::filesystem::exists(sp));
      REQUIRE(std::filesystem::path(sp).filename().string().rfind("strip_", 0) == 0);
    }
  }
  REQUIRE(saw_timing_slot);

  // recognize_strips (verifies Gray8->RGB24 normalization + per-item failure isolation)
  REQUIRE(results[15]["ok"] == true);
  REQUIRE(backend.last_ocr_input_format == sublift::PixelFormat::RGB24);
  const auto& strip_res = results[15]["data"]["results"];
  REQUIRE(strip_res.size() == 2);
  REQUIRE(strip_res[0]["ok"] == true);
  REQUIRE(strip_res[0]["text"] == "Mock Subtitle Line");
  REQUIRE(strip_res[1]["ok"] == false);
  REQUIRE(strip_res[1].contains("error_message"));
}

TEST_CASE("WorkerServer concurrency: deterministic cancel_task and stdin EOF shutdown",
          "[worker][unit]") {
  SECTION("cancel_task cancels in-flight run_extraction deterministically") {
    std::promise<void> entered_extract_promise;
    auto entered_extract_future = entered_extract_promise.get_future();
    std::atomic<bool> source_cancelled{false};

    MockWorkerBackend backend;
    backend.source_factory = [&](double, std::int32_t) {
      return std::make_unique<BlockingFrameSource>(
          entered_extract_promise, source_cancelled);
    };

    PipeStreamBuf in_buf;
    RecordingOutputStreamBuf out_buf;
    std::istream in(&in_buf);
    std::ostream out(&out_buf);

    sublift::worker::WorkerServer server(backend);
    int exit_code = -1;
    std::thread server_thread([&]() { exit_code = server.run(in, out); });

    in_buf.push_line(
        "{\"id\":101,\"op\":\"run_extraction\",\"params\":{"
        "\"video_path\":\"dummy.mp4\",\"mode\":\"local_ocr\",\"engine\":\"paddle\","
        "\"config\":{\"sample_fps\":5.0,\"subtitle_script\":\"auto\"}}}\n");

    // Deterministic wait until BlockingFrameSource::extract() is active
    entered_extract_future.wait();

    in_buf.push_line(
        "{\"id\":102,\"op\":\"cancel_task\",\"params\":{\"target_id\":101}}\n");
    in_buf.close_write();
    server_thread.join();

    REQUIRE(exit_code == 0);
    REQUIRE(source_cancelled.load());

    const auto msgs = out_buf.parsed_lines();
    bool saw_cancel_ok = false;
    bool saw_task_aborted = false;
    for (const auto& m : msgs) {
      if (m["type"] == "result" && m["id"] == 102) {
        REQUIRE(m["ok"] == true);
        REQUIRE(m["data"]["target_id"] == 101);
        REQUIRE(m["data"]["cancelled"] == true);
        saw_cancel_ok = true;
      }
      if (m["type"] == "result" && m["id"] == 101) {
        REQUIRE(m["ok"] == false);
        REQUIRE(m["error"]["code"] == "Aborted");
        saw_task_aborted = true;
      }
    }
    REQUIRE(saw_cancel_ok);
    REQUIRE(saw_task_aborted);
  }

  SECTION("stdin EOF cancels active task, joins threads, and exits 0") {
    std::promise<void> entered_extract_promise;
    auto entered_extract_future = entered_extract_promise.get_future();
    std::atomic<bool> source_cancelled{false};

    MockWorkerBackend backend;
    backend.source_factory = [&](double, std::int32_t) {
      return std::make_unique<BlockingFrameSource>(
          entered_extract_promise, source_cancelled);
    };

    PipeStreamBuf in_buf;
    RecordingOutputStreamBuf out_buf;
    std::istream in(&in_buf);
    std::ostream out(&out_buf);

    sublift::worker::WorkerServer server(backend);
    int exit_code = -1;
    std::thread server_thread([&]() { exit_code = server.run(in, out); });

    in_buf.push_line(
        "{\"id\":201,\"op\":\"run_extraction\",\"params\":{"
        "\"video_path\":\"dummy.mp4\",\"mode\":\"local_ocr\",\"engine\":\"paddle\","
        "\"config\":{\"sample_fps\":5.0,\"subtitle_script\":\"auto\"}}}\n");

    entered_extract_future.wait();
    // Immediately close stdin without sending cancel_task
    in_buf.close_write();
    server_thread.join();

    REQUIRE(exit_code == 0);
    REQUIRE(source_cancelled.load());

    const auto msgs = out_buf.parsed_lines();
    bool saw_task_aborted = false;
    for (const auto& m : msgs) {
      if (m["type"] == "result" && m["id"] == 201) {
        REQUIRE(m["ok"] == false);
        REQUIRE(m["error"]["code"] == "Aborted");
        saw_task_aborted = true;
      }
    }
    REQUIRE(saw_task_aborted);
  }

  SECTION("cancel_task while SegmentRecognizer worker is blocked mid-OCR converges cleanly without UAF") {
    std::promise<void> entered_ocr_promise;
    auto entered_ocr_future = entered_ocr_promise.get_future();
    std::promise<void> release_ocr_promise;
    auto release_ocr_future = release_ocr_promise.get_future().share();
    std::atomic<bool> source_cancelled{false};

    MockWorkerBackend backend;
    backend.ocr_override = std::make_shared<BlockingOcrEngine>(
        entered_ocr_promise, release_ocr_future);
    backend.source_factory = [&](double, std::int32_t) {
      return std::make_unique<MidOcrBlockingFrameSource>(source_cancelled);
    };

    PipeStreamBuf in_buf;
    RecordingOutputStreamBuf out_buf;
    std::mutex wait_mu;
    std::condition_variable wait_cv;
    std::unordered_map<std::uint64_t, json> results;
    std::vector<json> ordered_msgs;
    out_buf.on_line([&](const std::string& raw) {
      json j = json::parse(raw);
      {
        std::lock_guard<std::mutex> lock(wait_mu);
        ordered_msgs.push_back(j);
        if (j["type"] == "result") {
          results[j["id"].get<std::uint64_t>()] = j;
        }
      }
      wait_cv.notify_all();
    });

    std::istream in(&in_buf);
    std::ostream out(&out_buf);
    sublift::worker::WorkerServer server(backend);
    int exit_code = -1;
    std::thread server_thread([&]() { exit_code = server.run(in, out); });

    in_buf.push_line(
        "{\"id\":301,\"op\":\"run_extraction\",\"params\":{"
        "\"video_path\":\"dummy.mp4\",\"mode\":\"local_ocr\",\"engine\":\"paddle\","
        "\"config\":{\"sample_fps\":5.0,\"region_bottom_ratio\":0.25,\"subtitle_script\":\"auto\"}}}\n");

    // Deterministic wait until SegmentRecognizer::worker_ is inside IOcrEngine::recognize()
    entered_ocr_future.wait();

    // Trigger cancel_task while OCR is still blocked
    in_buf.push_line(
        "{\"id\":302,\"op\":\"cancel_task\",\"params\":{\"target_id\":301}}\n");
    {
      std::unique_lock<std::mutex> lock(wait_mu);
      wait_cv.wait(lock, [&]() { return results.find(302) != results.end(); });
    }

    // Unblock OCR after cancel_task has completed; ~ExtractionSession must join worker_
    // before emitting result 301 and before ~WorkerExtractionObserver runs.
    release_ocr_promise.set_value();
    {
      std::unique_lock<std::mutex> lock(wait_mu);
      wait_cv.wait(lock, [&]() { return results.find(301) != results.end(); });
    }

    in_buf.close_write();
    server_thread.join();

    REQUIRE(exit_code == 0);
    REQUIRE(source_cancelled.load());
    REQUIRE(results[302]["ok"] == true);
    REQUIRE(results[302]["data"]["target_id"] == 301);
    REQUIRE(results[302]["data"]["cancelled"] == true);
    REQUIRE(results[301]["ok"] == false);
    REQUIRE(results[301]["error"]["code"] == "Aborted");

    // Verify no late events for task 301 were emitted after result 301
    bool saw_result_301 = false;
    for (const auto& m : ordered_msgs) {
      if (m["id"] == 301 && m["type"] == "result") {
        saw_result_301 = true;
      } else if (m["id"] == 301 && m["type"] == "event") {
        REQUIRE_FALSE(saw_result_301);
      }
    }
    REQUIRE(saw_result_301);
  }

  SECTION("mid-stream exception in extract() joins consumer/OCR threads before observer destruction") {
    class ThrowingFrameSource final : public sublift::IFrameSource {
     public:
      sublift::VideoInfo probe(const std::filesystem::path&) override {
        return sublift::VideoInfo{
            .width = 640,
            .height = 360,
            .duration_ms = 3000,
            .fps = 25.0,
            .display_transform_ok = true,
        };
      }
      sublift::FrameIOPlan plan(
          const sublift::VideoInfo& i,
          const std::optional<sublift::SourceBox>& requested_box) override {
        return sublift::FrameIOPlan{
            .output_crop = requested_box,
            .output_mode = requested_box.has_value()
                               ? sublift::FrameOutputMode::RoiRgb
                               : sublift::FrameOutputMode::FullRgb,
            .source = sublift::SourceFrameInfo{.width = i.width, .height = i.height},
        };
      }
      void extract(
          const std::filesystem::path&,
          double,
          const std::optional<sublift::SourceBox>&,
          std::function<bool(sublift::Frame)> consumer) override {
        for (int i = 0; i < 15; ++i) {
          const bool has_sub = (i >= 3 && i <= 10);
          if (!consumer(sublift::Frame{
                  .timestamp_ms = i * 200,
                  .image = make_subtitle_frame(640, 180, has_sub),
              })) {
            return;
          }
        }
        throw std::runtime_error("simulated decode error");
      }
      sublift::ImageBuffer extract_frame(
          const std::filesystem::path&,
          std::int64_t,
          const std::optional<sublift::SourceBox>&) override {
        return sublift::ImageBuffer{};
      }
      void cancel() override { cancelled_.store(true, std::memory_order_release); }
      bool is_cancelled() const noexcept override {
        return cancelled_.load(std::memory_order_acquire);
      }

     private:
      std::atomic<bool> cancelled_{false};
    };

    MockWorkerBackend backend;
    backend.source_factory = [&](double, std::int32_t) {
      return std::make_unique<ThrowingFrameSource>();
    };

    PipeStreamBuf in_buf;
    RecordingOutputStreamBuf out_buf;
    std::mutex wait_mu;
    std::condition_variable wait_cv;
    bool got_401 = false;
    out_buf.on_line([&](const std::string& raw) {
      json j = json::parse(raw);
      if (j["type"] == "result" && j["id"] == 401) {
        {
          std::lock_guard<std::mutex> lock(wait_mu);
          got_401 = true;
        }
        wait_cv.notify_all();
      }
    });

    std::istream in(&in_buf);
    std::ostream out(&out_buf);
    sublift::worker::WorkerServer server(backend);
    int exit_code = -1;
    std::thread server_thread([&]() { exit_code = server.run(in, out); });

    in_buf.push_line(
        "{\"id\":401,\"op\":\"run_extraction\",\"params\":{"
        "\"video_path\":\"dummy.mp4\",\"mode\":\"local_ocr\",\"engine\":\"paddle\","
        "\"config\":{\"sample_fps\":5.0,\"region_bottom_ratio\":0.25,\"subtitle_script\":\"auto\"}}}\n");
    {
      std::unique_lock<std::mutex> lock(wait_mu);
      wait_cv.wait(lock, [&]() { return got_401; });
    }
    in_buf.close_write();
    server_thread.join();

    REQUIRE(exit_code == 0);
    bool saw_decoder_failed = false;
    for (const auto& m : out_buf.parsed_lines()) {
      if (m["type"] == "result" && m["id"] == 401) {
        REQUIRE(m["ok"] == false);
        REQUIRE(m["error"]["code"] == "DecoderFailed");
        REQUIRE(m["error"]["message"] == "simulated decode error");
        saw_decoder_failed = true;
      }
    }
    REQUIRE(saw_decoder_failed);
  }

  SECTION("pure_timing_strips re-creates source safely without dangling active_source on cancel_task") {
    TempDirGuard tmp("rebind_source");
    std::promise<void> entered_second_extract;
    auto entered_future = entered_second_extract.get_future();
    std::atomic<int> created_count{0};
    std::atomic<bool> first_cancelled{false};
    std::atomic<bool> second_cancelled{false};

    class RebindTestFrameSource final : public sublift::IFrameSource {
     public:
      RebindTestFrameSource(
          bool is_second,
          std::promise<void>& entered,
          std::atomic<bool>& cancelled_flag)
          : is_second_(is_second),
            entered_(entered),
            cancelled_flag_(cancelled_flag) {}

      sublift::VideoInfo probe(const std::filesystem::path&) override {
        return sublift::VideoInfo{
            .width = 640,
            .height = 360,
            .duration_ms = 2000,
            .fps = 25.0,
            .display_transform_ok = false,
        };
      }
      sublift::FrameIOPlan plan(
          const sublift::VideoInfo& i,
          const std::optional<sublift::SourceBox>& requested_box) override {
        return sublift::FrameIOPlan{
            .output_crop = requested_box,
            .output_mode = sublift::FrameOutputMode::FullRgb,
            .source = sublift::SourceFrameInfo{.width = i.width, .height = i.height},
        };
      }
      void extract(
          const std::filesystem::path&,
          double,
          const std::optional<sublift::SourceBox>&,
          std::function<bool(sublift::Frame)> consumer) override {
        consumer(sublift::Frame{0, make_subtitle_frame(640, 360, false)});
        if (is_second_ && !signaled_.exchange(true)) {
          entered_.set_value();
        }
        std::unique_lock<std::mutex> lock(mu_);
        cv_.wait(lock, [this]() { return cancelled_.load(std::memory_order_acquire); });
      }
      sublift::ImageBuffer extract_frame(
          const std::filesystem::path&,
          std::int64_t,
          const std::optional<sublift::SourceBox>&) override {
        return sublift::ImageBuffer{};
      }
      void cancel() override {
        {
          std::lock_guard<std::mutex> lock(mu_);
          cancelled_.store(true, std::memory_order_release);
          cancelled_flag_.store(true, std::memory_order_release);
        }
        cv_.notify_all();
      }
      bool is_cancelled() const noexcept override {
        return cancelled_.load(std::memory_order_acquire);
      }

     private:
      bool is_second_{false};
      std::promise<void>& entered_;
      std::atomic<bool>& cancelled_flag_;
      std::atomic<bool> signaled_{false};
      std::atomic<bool> cancelled_{false};
      std::mutex mu_;
      std::condition_variable cv_;
    };

    MockWorkerBackend backend;
    std::vector<std::int32_t> requested_max_widths;
    backend.source_factory = [&](double, std::int32_t max_w) {
      requested_max_widths.push_back(max_w);
      const int idx = ++created_count;
      return std::make_unique<RebindTestFrameSource>(
          idx == 2,
          entered_second_extract,
          idx == 1 ? first_cancelled : second_cancelled);
    };

    PipeStreamBuf in_buf;
    RecordingOutputStreamBuf out_buf;
    std::mutex wait_mu;
    std::condition_variable wait_cv;
    bool got_501 = false;
    bool got_502 = false;
    out_buf.on_line([&](const std::string& raw) {
      json j = json::parse(raw);
      if (j["type"] == "result") {
        {
          std::lock_guard<std::mutex> lock(wait_mu);
          if (j["id"] == 501) got_501 = true;
          if (j["id"] == 502) got_502 = true;
        }
        wait_cv.notify_all();
      }
    });

    std::istream in(&in_buf);
    std::ostream out(&out_buf);
    sublift::worker::WorkerServer server(backend);
    int exit_code = -1;
    std::thread server_thread([&]() { exit_code = server.run(in, out); });

    in_buf.push_line(
        json{{"id", 501},
             {"op", "run_extraction"},
             {"params",
              {{"video_path", "dummy.mp4"},
               {"mode", "pure_timing_strips"},
               {"strips_dir", (tmp.path / "strips").string()},
               {"region_box", {{"x", 0}, {"y", 180}, {"width", 640}, {"height", 180}}},
               {"config", {{"sample_fps", 5.0}, {"subtitle_script", "auto"}}}}}}
            .dump() +
        "\n");

    entered_future.wait();
    in_buf.push_line(
        "{\"id\":502,\"op\":\"cancel_task\",\"params\":{\"target_id\":501}}\n");
    {
      std::unique_lock<std::mutex> lock(wait_mu);
      wait_cv.wait(lock, [&]() { return got_501 && got_502; });
    }
    in_buf.close_write();
    server_thread.join();

    REQUIRE(exit_code == 0);
    REQUIRE(requested_max_widths == std::vector<std::int32_t>{1920, 0});
    REQUIRE_FALSE(first_cancelled.load());
    REQUIRE(second_cancelled.load());

    bool saw_aborted = false;
    for (const auto& m : out_buf.parsed_lines()) {
      if (m["type"] == "result" && m["id"] == 501) {
        REQUIRE(m["ok"] == false);
        REQUIRE(m["error"]["code"] == "Aborted");
        saw_aborted = true;
      }
    }
    REQUIRE(saw_aborted);
  }
}

TEST_CASE("WorkerServer recognize_strips: line-selection fallback parity with SegmentRecognizer",
          "[worker][unit]") {
  TempDirGuard tmp("recognize_strips_fallback");
  const auto strip_path = tmp.path / "strip.ppm";
  sublift::ImageBuffer rgb(120, 40, sublift::PixelFormat::RGB24);
  std::memset(rgb.data(), 10, 120U * 40U * 3U);
  sublift::worker::write_ppm_atomic(strip_path, rgb.view());

  MockWorkerBackend backend;
  backend.mock_ocr->set_sequence({
      // Call 1: non-empty lines rejected by select_line (Latin text under script='cjk')
      // Must NOT fall back to ocr_res.text!
      sublift::OcrResult{
          .text = "Watermark 123",
          .confidence = 0.91,
          .lines = {sublift::OcrLine{
              .text = "Watermark 123",
              .confidence = 0.91,
              .box = sublift::OcrCropBox{0, 0, 120, 40},
          }},
      },
      // Call 2: empty lines -> MUST fall back to cleanup_subtitle_text(ocr_res.text, script)
      sublift::OcrResult{
          .text = "  Fallback   Subtitle  ",
          .confidence = 0.88,
          .lines = {},
      },
  });

  PipeStreamBuf in_buf;
  RecordingOutputStreamBuf out_buf;
  std::mutex wait_mu;
  std::condition_variable wait_cv;
  std::unordered_map<std::uint64_t, json> results;
  out_buf.on_line([&](const std::string& raw) {
    json j = json::parse(raw);
    if (j["type"] == "result") {
      {
        std::lock_guard<std::mutex> lock(wait_mu);
        results[j["id"].get<std::uint64_t>()] = j;
      }
      wait_cv.notify_all();
    }
  });

  std::istream in(&in_buf);
  std::ostream out(&out_buf);
  sublift::worker::WorkerServer server(backend);
  int exit_code = -1;
  std::thread server_thread([&]() { exit_code = server.run(in, out); });

  in_buf.push_line(
      json{{"id", 601},
           {"op", "recognize_strips"},
           {"params",
            {{"engine", "paddle"},
             {"strip_paths", {strip_path.string()}},
             {"script", "cjk"}}}}
          .dump() +
      "\n");
  {
    std::unique_lock<std::mutex> lock(wait_mu);
    wait_cv.wait(lock, [&]() { return results.find(601) != results.end(); });
  }
  in_buf.push_line(
      json{{"id", 602},
           {"op", "recognize_strips"},
           {"params",
            {{"engine", "paddle"},
             {"strip_paths", {strip_path.string()}},
             {"script", "latin"}}}}
          .dump() +
      "\n");
  {
    std::unique_lock<std::mutex> lock(wait_mu);
    wait_cv.wait(lock, [&]() { return results.find(602) != results.end(); });
  }
  in_buf.close_write();
  server_thread.join();

  REQUIRE(exit_code == 0);
  REQUIRE(results[601]["ok"] == true);
  REQUIRE(results[601]["data"]["results"][0]["ok"] == true);
  REQUIRE(results[601]["data"]["results"][0]["text"] == "");
  REQUIRE(results[601]["data"]["results"][0]["confidence"] == Catch::Approx(0.0));

  REQUIRE(results[602]["ok"] == true);
  REQUIRE(results[602]["data"]["results"][0]["ok"] == true);
  REQUIRE(results[602]["data"]["results"][0]["text"] == "Fallback Subtitle");
  REQUIRE(results[602]["data"]["results"][0]["confidence"] == Catch::Approx(0.88));
}

TEST_CASE("WorkerServer Tier-5 boundaries: malformed shapes, finalize_srt invariants, validation & mid-flight cancels",
          "[worker][unit]") {
  TempDirGuard tmp("tier5_boundaries");

  SECTION("finalize_srt post-clamp min_duration_ms invariant, negative options, empty/whitespace & missing entries") {
    MockWorkerBackend backend;
    sublift::worker::WorkerServer server(backend);
    PipeStreamBuf in_buf;
    RecordingOutputStreamBuf out_buf;
    std::mutex wait_mu;
    std::condition_variable wait_cv;
    std::vector<json> msgs;
    out_buf.on_line([&](const std::string& raw) {
      {
        std::lock_guard<std::mutex> lock(wait_mu);
        msgs.push_back(json::parse(raw));
      }
      wait_cv.notify_all();
    });

    std::istream in(&in_buf);
    std::ostream out(&out_buf);
    int exit_code = -1;
    std::thread server_thread([&]() { exit_code = server.run(in, out); });

    in_buf.push_line(
        // 1: start_ms < 0 clamped before min_duration_ms filter (400 - 0 = 400 < 500 -> filtered out)
        "{\"id\":1,\"op\":\"finalize_srt\",\"params\":{\"apply_merge\":true,\"min_duration_ms\":500,\"entries\":[{\"id\":1,\"start_ms\":-250,\"end_ms\":400,\"text\":\"ClampedShort\"}]}}\n"
        // 2: empty entries
        "{\"id\":2,\"op\":\"finalize_srt\",\"params\":{\"apply_merge\":true,\"entries\":[]}}\n"
        // 3: whitespace-only text
        "{\"id\":3,\"op\":\"finalize_srt\",\"params\":{\"apply_merge\":false,\"entries\":[{\"id\":1,\"start_ms\":0,\"end_ms\":1000,\"text\":\"  \\n\\t  \"}]}}\n"
        // 4: missing entries key
        "{\"id\":4,\"op\":\"finalize_srt\",\"params\":{\"apply_merge\":false}}\n"
        // 5: negative min_duration_ms
        "{\"id\":5,\"op\":\"finalize_srt\",\"params\":{\"apply_merge\":true,\"min_duration_ms\":-1,\"entries\":[]}}\n"
        // 6: negative merge_gap_ms
        "{\"id\":6,\"op\":\"finalize_srt\",\"params\":{\"apply_merge\":true,\"merge_gap_ms\":-1,\"entries\":[]}}\n"
        // 7..13: malformed request shapes
        "[]\n"
        "123\n"
        "null\n"
        "{\"id\":-1,\"op\":\"capabilities\",\"params\":{}}\n"
        "{\"id\":1.5,\"op\":\"capabilities\",\"params\":{}}\n"
        "{\"id\":10,\"op\":\"capabilities\",\"params\":null}\n"
        "{\"id\":11,\"op\":\"capabilities\",\"params\":[]}\n"
        "{\"id\":0,\"op\":\"capabilities\",\"params\":{}}\n"
        // 15: cancel_task unknown target_id & invalid target_id
        "{\"id\":12,\"op\":\"cancel_task\",\"params\":{\"target_id\":999999}}\n"
        "{\"id\":13,\"op\":\"cancel_task\",\"params\":{\"target_id\":-5}}\n"
        "{\"id\":14,\"op\":\"cancel_task\",\"params\":{}}\n");

    {
      std::unique_lock<std::mutex> lock(wait_mu);
      wait_cv.wait(lock, [&]() { return msgs.size() >= 17; });
    }
    in_buf.close_write();
    server_thread.join();
    REQUIRE(exit_code == 0);

    const auto by_id = [&](std::uint64_t id) -> json {
      for (const auto& m : msgs) {
        if (m["id"] == id && m.value("type", "") == "result") {
          return m;
        }
      }
      FAIL("missing response for id");
      return {};
    };

    REQUIRE(by_id(1)["ok"] == true);
    REQUIRE(by_id(1)["data"]["cue_count"] == 0);
    REQUIRE(by_id(1)["data"]["srt_text"] == "");

    REQUIRE(by_id(2)["ok"] == true);
    REQUIRE(by_id(2)["data"]["cue_count"] == 0);
    REQUIRE(by_id(2)["data"]["srt_text"] == "");

    REQUIRE(by_id(3)["ok"] == true);
    REQUIRE(by_id(3)["data"]["cue_count"] == 0);
    REQUIRE(by_id(3)["data"]["srt_text"] == "");

    REQUIRE(by_id(4)["ok"] == false);
    REQUIRE(by_id(4)["error"]["code"] == "InvalidRequest");

    REQUIRE(by_id(5)["ok"] == false);
    REQUIRE(by_id(5)["error"]["code"] == "InvalidConfiguration");

    REQUIRE(by_id(6)["ok"] == false);
    REQUIRE(by_id(6)["error"]["code"] == "InvalidConfiguration");

    // id 10, 11 -> InvalidRequest; id 12 -> cancelled:false; id 13, 14 -> InvalidRequest
    REQUIRE(by_id(10)["ok"] == false);
    REQUIRE(by_id(10)["error"]["code"] == "InvalidRequest");
    REQUIRE(by_id(11)["ok"] == false);
    REQUIRE(by_id(11)["error"]["code"] == "InvalidRequest");
    REQUIRE(by_id(12)["ok"] == true);
    REQUIRE(by_id(12)["data"]["cancelled"] == false);
    REQUIRE(by_id(13)["ok"] == false);
    REQUIRE(by_id(13)["error"]["code"] == "InvalidRequest");
    REQUIRE(by_id(14)["ok"] == false);
    REQUIRE(by_id(14)["error"]["code"] == "InvalidRequest");

    // id == 0 valid capabilities request + 5 malformed lines that fall back to id == 0 errors
    int id0_ok = 0;
    int id0_err = 0;
    for (const auto& m : msgs) {
      if (m["id"] == 0) {
        if (m["ok"] == true) {
          ++id0_ok;
        } else {
          REQUIRE(m["error"]["code"] == "InvalidRequest");
          ++id0_err;
        }
      }
    }
    REQUIRE(id0_ok == 1);
    REQUIRE(id0_err == 5);
  }

  SECTION("extract_frame, run_extraction & recognize_strips parameter validation and IoError mapping") {
    MockWorkerBackend backend;
    sublift::worker::WorkerServer server(backend);
    PipeStreamBuf in_buf;
    RecordingOutputStreamBuf out_buf;
    std::mutex wait_mu;
    std::condition_variable wait_cv;
    std::unordered_map<std::uint64_t, json> results;
    out_buf.on_line([&](const std::string& raw) {
      json j = json::parse(raw);
      if (j["type"] == "result") {
        {
          std::lock_guard<std::mutex> lock(wait_mu);
          results[j["id"].get<std::uint64_t>()] = j;
        }
        wait_cv.notify_all();
      }
    });

    std::istream in(&in_buf);
    std::ostream out(&out_buf);
    int exit_code = -1;
    std::thread server_thread([&]() { exit_code = server.run(in, out); });

    const auto existing_dir = tmp.path / "existing_dir.png";
    std::filesystem::create_directories(existing_dir);

    const std::vector<json> reqs = {
        {{"id", 21}, {"op", "extract_frame"}, {"params", {{"video_path", "v.mp4"}, {"timestamp_ms", -1}, {"out_path", (tmp.path / "a.png").string()}}}},
        {{"id", 22}, {"op", "extract_frame"}, {"params", {{"video_path", "v.mp4"}, {"timestamp_ms", 0}, {"out_path", (tmp.path / "a.jpg").string()}}}},
        {{"id", 23}, {"op", "extract_frame"}, {"params", {{"video_path", "v.mp4"}, {"timestamp_ms", 0}, {"crop", {{"x", 0}, {"y", 0}, {"width", 0}, {"height", 10}}}, {"out_path", (tmp.path / "b.png").string()}}}},
        {{"id", 24}, {"op", "extract_frame"}, {"params", {{"video_path", "v.mp4"}, {"timestamp_ms", 0}, {"crop", {{"x", -1}, {"y", 0}, {"width", 10}, {"height", 10}}}, {"out_path", (tmp.path / "c.png").string()}}}},
        {{"id", 25}, {"op", "extract_frame"}, {"params", {{"video_path", "v.mp4"}, {"timestamp_ms", 0}, {"max_width", -5}, {"out_path", (tmp.path / "d.png").string()}}}},
        {{"id", 26}, {"op", "extract_frame"}, {"params", {{"video_path", "v.mp4"}, {"timestamp_ms", 0}, {"out_path", existing_dir.string()}}}},
        {{"id", 27}, {"op", "run_extraction"}, {"params", {{"video_path", "v.mp4"}, {"mode", "local_ocr"}, {"engine", "paddle"}, {"config", {{"min_duration_ms", -1}}}}}},
        {{"id", 28}, {"op", "run_extraction"}, {"params", {{"video_path", "v.mp4"}, {"mode", "local_ocr"}, {"engine", "paddle"}, {"config", {{"merge_gap_ms", -1}}}}}},
        {{"id", 29}, {"op", "run_extraction"}, {"params", {{"video_path", "v.mp4"}, {"mode", "local_ocr"}, {"engine", "paddle"}, {"config", {{"confidence_threshold", 1.5}}}}}},
        {{"id", 30}, {"op", "run_extraction"}, {"params", {{"video_path", "v.mp4"}, {"mode", "local_ocr"}, {"engine", "paddle"}, {"config", {{"confidence_threshold", -0.1}}}}}},
        {{"id", 31}, {"op", "recognize_strips"}, {"params", {{"engine", "paddle"}, {"strip_paths", json::array()}, {"script", "bogus"}}}},
        {{"id", 32}, {"op", "recognize_strips"}, {"params", {{"engine", "paddle"}, {"strip_paths", {123}}, {"script", "auto"}}}},
    };
    for (const auto& r : reqs) {
      in_buf.push_line(r.dump() + "\n");
    }
    {
      std::unique_lock<std::mutex> lock(wait_mu);
      wait_cv.wait(lock, [&]() { return results.size() >= reqs.size(); });
    }
    in_buf.close_write();
    server_thread.join();
    REQUIRE(exit_code == 0);

    for (std::uint64_t id : {21ULL, 22ULL, 23ULL, 24ULL, 25ULL, 27ULL, 28ULL, 29ULL, 30ULL, 31ULL}) {
      REQUIRE(results[id]["ok"] == false);
      REQUIRE(results[id]["error"]["code"] == "InvalidConfiguration");
    }
    REQUIRE(results[26]["ok"] == false);
    REQUIRE(results[26]["error"]["code"] == "IoError");
    REQUIRE(results[32]["ok"] == false);
    REQUIRE(results[32]["error"]["code"] == "InvalidRequest");
  }

  SECTION("duplicate active async id is rejected while original task completes on cancel") {
    std::promise<void> entered_promise;
    auto entered_future = entered_promise.get_future();
    std::atomic<bool> cancelled{false};

    MockWorkerBackend backend;
    backend.source_factory = [&](double, std::int32_t) {
      return std::make_unique<BlockingFrameSource>(entered_promise, cancelled);
    };

    PipeStreamBuf in_buf;
    RecordingOutputStreamBuf out_buf;
    std::mutex wait_mu;
    std::condition_variable wait_cv;
    std::vector<json> msgs;
    out_buf.on_line([&](const std::string& raw) {
      {
        std::lock_guard<std::mutex> lock(wait_mu);
        msgs.push_back(json::parse(raw));
      }
      wait_cv.notify_all();
    });

    std::istream in(&in_buf);
    std::ostream out(&out_buf);
    sublift::worker::WorkerServer server(backend);
    int exit_code = -1;
    std::thread server_thread([&]() { exit_code = server.run(in, out); });

    in_buf.push_line(
        "{\"id\":701,\"op\":\"run_extraction\",\"params\":{"
        "\"video_path\":\"v.mp4\",\"mode\":\"local_ocr\",\"engine\":\"paddle\"}}\n");
    entered_future.wait();

    // Send duplicate async id 701 while original 701 is still active
    in_buf.push_line(
        "{\"id\":701,\"op\":\"probe_video\",\"params\":{\"video_path\":\"v.mp4\"}}\n");
    {
      std::unique_lock<std::mutex> lock(wait_mu);
      wait_cv.wait(lock, [&]() {
        for (const auto& m : msgs) {
          if (m["id"] == 701 && m.value("type", "") == "result" &&
              m["error"]["code"] == "InvalidRequest") {
            return true;
          }
        }
        return false;
      });
    }

    // Now cancel original 701
    in_buf.push_line("{\"id\":702,\"op\":\"cancel_task\",\"params\":{\"target_id\":701}}\n");
    {
      std::unique_lock<std::mutex> lock(wait_mu);
      wait_cv.wait(lock, [&]() {
        bool saw_aborted = false;
        bool saw_cancel = false;
        for (const auto& m : msgs) {
          if (m["id"] == 701 && m.value("type", "") == "result" &&
              m["error"]["code"] == "Aborted") {
            saw_aborted = true;
          }
          if (m["id"] == 702 && m.value("type", "") == "result") {
            saw_cancel = true;
          }
        }
        return saw_aborted && saw_cancel;
      });
    }
    in_buf.close_write();
    server_thread.join();
    REQUIRE(exit_code == 0);
    REQUIRE(cancelled.load());
  }

  SECTION("mid-flight cancellation for probe_video, extract_frame, detect_auto_region, and recognize_strips") {
    class CancellableBlockingSource final : public sublift::IFrameSource {
     public:
      enum class BlockIn { Probe, ExtractFrame };
      CancellableBlockingSource(
          BlockIn where,
          std::promise<void>& entered,
          std::atomic<bool>& cancelled_flag)
          : where_(where), entered_(entered), cancelled_flag_(cancelled_flag) {}

      sublift::VideoInfo probe(const std::filesystem::path&) override {
        if (where_ == BlockIn::Probe) {
          if (!signaled_.exchange(true)) {
            entered_.set_value();
          }
          std::unique_lock<std::mutex> lock(mu_);
          cv_.wait(lock, [this]() { return cancelled_.load(std::memory_order_acquire); });
        }
        return sublift::VideoInfo{.width = 640, .height = 360, .duration_ms = 2000, .fps = 25.0};
      }
      sublift::FrameIOPlan plan(
          const sublift::VideoInfo& i,
          const std::optional<sublift::SourceBox>& requested_box) override {
        return sublift::FrameIOPlan{
            .output_crop = requested_box,
            .output_mode = sublift::FrameOutputMode::FullRgb,
            .source = sublift::SourceFrameInfo{.width = i.width, .height = i.height},
        };
      }
      void extract(
          const std::filesystem::path&,
          double,
          const std::optional<sublift::SourceBox>&,
          std::function<bool(sublift::Frame)>) override {}
      sublift::ImageBuffer extract_frame(
          const std::filesystem::path&,
          std::int64_t,
          const std::optional<sublift::SourceBox>&) override {
        if (where_ == BlockIn::ExtractFrame) {
          if (!signaled_.exchange(true)) {
            entered_.set_value();
          }
          std::unique_lock<std::mutex> lock(mu_);
          cv_.wait(lock, [this]() { return cancelled_.load(std::memory_order_acquire); });
        }
        return sublift::ImageBuffer{};
      }
      void cancel() override {
        {
          std::lock_guard<std::mutex> lock(mu_);
          cancelled_.store(true, std::memory_order_release);
          cancelled_flag_.store(true, std::memory_order_release);
        }
        cv_.notify_all();
      }
      bool is_cancelled() const noexcept override {
        return cancelled_.load(std::memory_order_acquire);
      }

     private:
      BlockIn where_;
      std::promise<void>& entered_;
      std::atomic<bool>& cancelled_flag_;
      std::atomic<bool> signaled_{false};
      std::atomic<bool> cancelled_{false};
      std::mutex mu_;
      std::condition_variable cv_;
    };

    // 1. probe_video & detect_auto_region (blocked in probe) & extract_frame (blocked in extract_frame)
    for (int mode = 0; mode < 3; ++mode) {
      std::promise<void> entered_p;
      auto entered_f = entered_p.get_future();
      std::atomic<bool> cancelled{false};
      MockWorkerBackend backend;
      backend.source_factory = [&, mode](double, std::int32_t) {
        const auto where = (mode == 1)
            ? CancellableBlockingSource::BlockIn::ExtractFrame
            : CancellableBlockingSource::BlockIn::Probe;
        return std::make_unique<CancellableBlockingSource>(where, entered_p, cancelled);
      };

      PipeStreamBuf in_buf;
      RecordingOutputStreamBuf out_buf;
      std::mutex wait_mu;
      std::condition_variable wait_cv;
      std::unordered_map<std::uint64_t, json> results;
      out_buf.on_line([&](const std::string& raw) {
        json j = json::parse(raw);
        if (j["type"] == "result") {
          {
            std::lock_guard<std::mutex> lock(wait_mu);
            results[j["id"].get<std::uint64_t>()] = j;
          }
          wait_cv.notify_all();
        }
      });

      std::istream in(&in_buf);
      std::ostream out(&out_buf);
      sublift::worker::WorkerServer server(backend);
      int exit_code = -1;
      std::thread server_thread([&]() { exit_code = server.run(in, out); });

      if (mode == 0) {
        in_buf.push_line("{\"id\":801,\"op\":\"probe_video\",\"params\":{\"video_path\":\"v.mp4\"}}\n");
      } else if (mode == 1) {
        in_buf.push_line(json{
            {"id", 801},
            {"op", "extract_frame"},
            {"params", {{"video_path", "v.mp4"}, {"timestamp_ms", 0}, {"out_path", (tmp.path / "x.png").string()}}},
        }.dump() + "\n");
      } else {
        in_buf.push_line("{\"id\":801,\"op\":\"detect_auto_region\",\"params\":{\"video_path\":\"v.mp4\",\"engine\":\"paddle\"}}\n");
      }
      entered_f.wait();
      in_buf.push_line("{\"id\":802,\"op\":\"cancel_task\",\"params\":{\"target_id\":801}}\n");
      {
        std::unique_lock<std::mutex> lock(wait_mu);
        wait_cv.wait(lock, [&]() { return results.count(801) && results.count(802); });
      }
      in_buf.close_write();
      server_thread.join();
      REQUIRE(exit_code == 0);
      REQUIRE(cancelled.load());
      REQUIRE(results[802]["data"]["cancelled"] == true);
      REQUIRE(results[801]["ok"] == false);
      REQUIRE(results[801]["error"]["code"] == "Aborted");
    }

    // 2. recognize_strips mid-batch cancel between strip 0 and strip 1
    const auto s0 = tmp.path / "s0.ppm";
    const auto s1 = tmp.path / "s1.ppm";
    sublift::ImageBuffer strip_buf(64, 24, sublift::PixelFormat::RGB24);
    std::memset(strip_buf.data(), 120, 64U * 24U * 3U);
    sublift::worker::write_ppm_atomic(s0, strip_buf.view());
    sublift::worker::write_ppm_atomic(s1, strip_buf.view());

    std::promise<void> entered_ocr_p;
    auto entered_ocr_f = entered_ocr_p.get_future();
    std::promise<void> release_ocr_p;
    auto release_ocr_f = release_ocr_p.get_future().share();
    std::atomic<int> recognize_count{0};

    class CountingBlockingOcr final : public sublift::IOcrEngine {
     public:
      CountingBlockingOcr(std::promise<void>& entered,
                          std::shared_future<void> release,
                          std::atomic<int>& count)
          : entered_(entered), release_(std::move(release)), count_(count) {}
      sublift::OcrResult recognize(const sublift::ImageView&) override {
        if (count_.fetch_add(1) == 0) {
          entered_.set_value();
          release_.wait();
        }
        return sublift::OcrResult{.text = "Strip0", .confidence = 0.9};
      }
      std::string engine_name() const override { return "counting_blocking"; }

     private:
      std::promise<void>& entered_;
      std::shared_future<void> release_;
      std::atomic<int>& count_;
    };

    MockWorkerBackend backend;
    backend.ocr_override = std::make_shared<CountingBlockingOcr>(
        entered_ocr_p, release_ocr_f, recognize_count);

    PipeStreamBuf in_buf;
    RecordingOutputStreamBuf out_buf;
    std::mutex wait_mu;
    std::condition_variable wait_cv;
    std::unordered_map<std::uint64_t, json> results;
    out_buf.on_line([&](const std::string& raw) {
      json j = json::parse(raw);
      if (j["type"] == "result") {
        {
          std::lock_guard<std::mutex> lock(wait_mu);
          results[j["id"].get<std::uint64_t>()] = j;
        }
        wait_cv.notify_all();
      }
    });

    std::istream in(&in_buf);
    std::ostream out(&out_buf);
    sublift::worker::WorkerServer server(backend);
    int exit_code = -1;
    std::thread server_thread([&]() { exit_code = server.run(in, out); });

    in_buf.push_line(json{
        {"id", 901},
        {"op", "recognize_strips"},
        {"params", {{"engine", "paddle"}, {"strip_paths", {s0.string(), s1.string()}}, {"script", "latin"}}},
    }.dump() + "\n");
    entered_ocr_f.wait();
    in_buf.push_line("{\"id\":902,\"op\":\"cancel_task\",\"params\":{\"target_id\":901}}\n");
    {
      std::unique_lock<std::mutex> lock(wait_mu);
      wait_cv.wait(lock, [&]() { return results.count(902) > 0; });
    }
    release_ocr_p.set_value();
    {
      std::unique_lock<std::mutex> lock(wait_mu);
      wait_cv.wait(lock, [&]() { return results.count(901) > 0; });
    }
    in_buf.close_write();
    server_thread.join();
    REQUIRE(exit_code == 0);
    REQUIRE(recognize_count.load() == 1);  // strip 1 must NOT be processed
    REQUIRE(results[901]["ok"] == false);
    REQUIRE(results[901]["error"]["code"] == "Aborted");
  }
}
