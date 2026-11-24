#include "image_io.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstring>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <vector>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

namespace sublift::worker::detail {

std::atomic<std::uint64_t>& temp_seq() noexcept {
  static std::atomic<std::uint64_t> seq{0};
  return seq;
}

long long current_pid() noexcept {
#if defined(_WIN32)
  return static_cast<long long>(_getpid());
#else
  return static_cast<long long>(::getpid());
#endif
}

[[nodiscard]] std::uint32_t crc32_ieee(const std::uint8_t* data, std::size_t len) noexcept {
  std::uint32_t crc = 0xFFFFFFFFU;
  for (std::size_t i = 0; i < len; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      const std::uint32_t mask = -(crc & 1U);
      crc = (crc >> 1U) ^ (0xEDB88320U & mask);
    }
  }
  return ~crc;
}

[[nodiscard]] std::uint32_t adler32(const std::uint8_t* data, std::size_t len) noexcept {
  constexpr std::uint32_t kModAdler = 65521U;
  std::uint32_t s1 = 1U;
  std::uint32_t s2 = 0U;
  for (std::size_t i = 0; i < len; ++i) {
    s1 = (s1 + data[i]) % kModAdler;
    s2 = (s2 + s1) % kModAdler;
  }
  return (s2 << 16U) | s1;
}

void append_be32(std::vector<std::uint8_t>& out, std::uint32_t v) {
  out.push_back(static_cast<std::uint8_t>((v >> 24U) & 0xFFU));
  out.push_back(static_cast<std::uint8_t>((v >> 16U) & 0xFFU));
  out.push_back(static_cast<std::uint8_t>((v >> 8U) & 0xFFU));
  out.push_back(static_cast<std::uint8_t>(v & 0xFFU));
}

void append_png_chunk(
    std::vector<std::uint8_t>& out,
    const char tag[4],
    const std::uint8_t* payload,
    std::size_t payload_len) {
  append_be32(out, static_cast<std::uint32_t>(payload_len));
  const std::size_t tag_offset = out.size();
  out.insert(out.end(), tag, tag + 4);
  if (payload_len > 0 && payload != nullptr) {
    out.insert(out.end(), payload, payload + payload_len);
  }
  const std::uint32_t crc =
      crc32_ieee(out.data() + tag_offset, 4U + payload_len);
  append_be32(out, crc);
}

void write_bytes_atomic(
    const std::filesystem::path& out_path,
    const std::uint8_t* data,
    std::size_t size) {
  const auto parent = out_path.parent_path();
  if (!parent.empty()) {
    std::error_code ec;
    std::filesystem::create_directories(parent, ec);
    if (ec) {
      throw std::runtime_error(
          "Failed to create directory " + parent.string() + ": " + ec.message());
    }
  }

  const std::uint64_t seq = temp_seq().fetch_add(1, std::memory_order_relaxed);
  const std::string tmp_name =
      ".tmp_" + std::to_string(current_pid()) + "_" + std::to_string(seq) + "_" +
      out_path.filename().string();
  const std::filesystem::path tmp_path = parent.empty()
      ? std::filesystem::path(tmp_name)
      : (parent / tmp_name);

  {
    std::ofstream ofs(tmp_path, std::ios::binary | std::ios::trunc);
    if (!ofs.is_open()) {
      throw std::runtime_error("Failed to open temp file for writing: " + tmp_path.string());
    }
    if (size > 0) {
      ofs.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    }
    ofs.flush();
    if (!ofs.good()) {
      ofs.close();
      std::error_code rm_ec;
      std::filesystem::remove(tmp_path, rm_ec);
      throw std::runtime_error("Failed to write temp file: " + tmp_path.string());
    }
  }

  std::error_code rename_ec;
  std::filesystem::rename(tmp_path, out_path, rename_ec);
  if (rename_ec) {
    std::error_code rm_ec;
    std::filesystem::remove(tmp_path, rm_ec);
    throw std::runtime_error(
        "Failed to rename temp file to " + out_path.string() + ": " + rename_ec.message());
  }
}

[[nodiscard]] std::string read_ppm_token(std::istream& is, int* out_delim = nullptr) {
  std::string token;
  int ch = is.get();
  while (ch != std::char_traits<char>::eof()) {
    if (ch == '#') {
      while (ch != std::char_traits<char>::eof() && ch != '\n') {
        ch = is.get();
      }
      if (ch == std::char_traits<char>::eof()) {
        break;
      }
      ch = is.get();
      continue;
    }
    if (!std::isspace(static_cast<unsigned char>(ch))) {
      break;
    }
    ch = is.get();
  }
  while (ch != std::char_traits<char>::eof() &&
         !std::isspace(static_cast<unsigned char>(ch)) &&
         ch != '#') {
    token.push_back(static_cast<char>(ch));
    ch = is.get();
  }
  if (out_delim != nullptr) {
    *out_delim = ch;
  }
  if (ch == '#') {
    is.unget();
  }
  return token;
}

[[nodiscard]] std::int32_t parse_strict_positive_int(
    const std::string& tok, const std::filesystem::path& path) {
  if (tok.empty()) {
    throw std::runtime_error("Empty PPM integer token in " + path.string());
  }
  std::uint64_t val = 0;
  for (unsigned char c : tok) {
    if (!std::isdigit(c)) {
      throw std::runtime_error(
          "Invalid PPM integer token '" + tok + "' in " + path.string());
    }
    val = val * 10ULL + static_cast<std::uint64_t>(c - '0');
    if (val > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max())) {
      throw std::runtime_error("PPM integer out of range in " + path.string());
    }
  }
  if (val == 0) {
    throw std::runtime_error("Out-of-range PPM header value (0) in " + path.string());
  }
  return static_cast<std::int32_t>(val);
}

}  // namespace sublift::worker::detail

namespace sublift::worker {

ImageBuffer resize_max_width(const ImageView& src, std::int32_t max_width) {
  if (src.empty()) {
    return ImageBuffer{};
  }
  const std::int32_t src_w = src.width();
  const std::int32_t src_h = src.height();
  const PixelFormat fmt = src.format();
  const std::int32_t bpp = bytes_per_pixel(fmt);

  if (max_width <= 0 || src_w <= max_width) {
    ImageBuffer copy = ImageBuffer::allocate_uninitialized(src_w, src_h, fmt);
    const std::size_t row_bytes = static_cast<std::size_t>(src_w) * static_cast<std::size_t>(bpp);
    for (std::int32_t y = 0; y < src_h; ++y) {
      std::memcpy(copy.row(y), src.row(y), row_bytes);
    }
    return copy;
  }

  const std::int32_t dst_w = max_width;
  const std::int32_t dst_h = std::max<std::int32_t>(
      1,
      static_cast<std::int32_t>(std::lround(
          static_cast<double>(src_h) * static_cast<double>(dst_w) /
          static_cast<double>(src_w))));

  ImageBuffer dst = ImageBuffer::allocate_uninitialized(dst_w, dst_h, fmt);
  const double scale_x = static_cast<double>(src_w) / static_cast<double>(dst_w);
  const double scale_y = static_cast<double>(src_h) / static_cast<double>(dst_h);

  for (std::int32_t dy = 0; dy < dst_h; ++dy) {
    const double sy = std::clamp((static_cast<double>(dy) + 0.5) * scale_y - 0.5,
                                 0.0, static_cast<double>(src_h - 1));
    const auto y0 = static_cast<std::int32_t>(std::floor(sy));
    const auto y1 = std::min<std::int32_t>(src_h - 1, y0 + 1);
    const double wy1 = sy - static_cast<double>(y0);
    const double wy0 = 1.0 - wy1;

    const std::uint8_t* row0 = src.row(y0);
    const std::uint8_t* row1 = src.row(y1);
    std::uint8_t* dst_row = dst.row(dy);

    for (std::int32_t dx = 0; dx < dst_w; ++dx) {
      const double sx = std::clamp((static_cast<double>(dx) + 0.5) * scale_x - 0.5,
                                   0.0, static_cast<double>(src_w - 1));
      const auto x0 = static_cast<std::int32_t>(std::floor(sx));
      const auto x1 = std::min<std::int32_t>(src_w - 1, x0 + 1);
      const double wx1 = sx - static_cast<double>(x0);
      const double wx0 = 1.0 - wx1;

      for (std::int32_t c = 0; c < bpp; ++c) {
        const double p00 = row0[x0 * bpp + c];
        const double p01 = row0[x1 * bpp + c];
        const double p10 = row1[x0 * bpp + c];
        const double p11 = row1[x1 * bpp + c];
        const double val =
            (p00 * wx0 + p01 * wx1) * wy0 + (p10 * wx0 + p11 * wx1) * wy1;
        dst_row[dx * bpp + c] = static_cast<std::uint8_t>(
            std::clamp(static_cast<int>(std::lround(val)), 0, 255));
      }
    }
  }
  return dst;
}

ImageBuffer ensure_rgb24(const ImageView& src) {
  if (src.empty()) {
    return ImageBuffer{};
  }
  const std::int32_t w = src.width();
  const std::int32_t h = src.height();
  ImageBuffer out = ImageBuffer::allocate_uninitialized(w, h, PixelFormat::RGB24);
  if (src.format() == PixelFormat::RGB24) {
    const std::size_t row_bytes = static_cast<std::size_t>(w) * 3U;
    for (std::int32_t y = 0; y < h; ++y) {
      std::memcpy(out.row(y), src.row(y), row_bytes);
    }
  } else {
    for (std::int32_t y = 0; y < h; ++y) {
      const std::uint8_t* src_row = src.row(y);
      std::uint8_t* dst_row = out.row(y);
      for (std::int32_t x = 0; x < w; ++x) {
        const std::uint8_t g = src_row[x];
        dst_row[x * 3 + 0] = g;
        dst_row[x * 3 + 1] = g;
        dst_row[x * 3 + 2] = g;
      }
    }
  }
  return out;
}

void write_ppm_atomic(const std::filesystem::path& out_path, const ImageView& image) {
  if (image.empty() || image.width() <= 0 || image.height() <= 0) {
    throw std::runtime_error("Cannot write empty image to PPM");
  }
  const int bpp = bytes_per_pixel(image.format());
  const char* magic = (image.format() == PixelFormat::RGB24) ? "P6" : "P5";
  const std::string header =
      std::string(magic) + "\n" + std::to_string(image.width()) + " " +
      std::to_string(image.height()) + "\n255\n";
  const std::size_t row_bytes =
      static_cast<std::size_t>(image.width()) * static_cast<std::size_t>(bpp);

  std::vector<std::uint8_t> bytes;
  bytes.reserve(header.size() + row_bytes * static_cast<std::size_t>(image.height()));
  bytes.insert(bytes.end(), header.begin(), header.end());
  for (std::int32_t y = 0; y < image.height(); ++y) {
    const std::uint8_t* row = image.row(y);
    bytes.insert(bytes.end(), row, row + row_bytes);
  }
  detail::write_bytes_atomic(out_path, bytes.data(), bytes.size());
}

void write_png_atomic(const std::filesystem::path& out_path, const ImageView& image) {
  if (image.empty() || image.width() <= 0 || image.height() <= 0) {
    throw std::runtime_error("Cannot write empty image to PNG");
  }

  const std::uint32_t w = static_cast<std::uint32_t>(image.width());
  const std::uint32_t h = static_cast<std::uint32_t>(image.height());
  const int bpp = bytes_per_pixel(image.format());
  const std::uint8_t color_type = (image.format() == PixelFormat::RGB24) ? 2U : 0U;
  const std::size_t row_bytes =
      static_cast<std::size_t>(w) * static_cast<std::size_t>(bpp);

  // Build unfiltered scanlines (1 filter byte 0x00 + row_bytes per scanline)
  std::vector<std::uint8_t> raw;
  raw.reserve(static_cast<std::size_t>(h) * (1U + row_bytes));
  for (std::int32_t y = 0; y < image.height(); ++y) {
    raw.push_back(0x00U);
    const std::uint8_t* row = image.row(y);
    raw.insert(raw.end(), row, row + row_bytes);
  }

  // Zlib stream using RFC 1951 stored deflate blocks (BTYPE=00)
  constexpr std::size_t kMaxStoredBlock = 65535U;
  const std::size_t block_count =
      std::max<std::size_t>(1U, (raw.size() + kMaxStoredBlock - 1U) / kMaxStoredBlock);

  std::vector<std::uint8_t> zlib_stream;
  zlib_stream.reserve(2U + block_count * 5U + raw.size() + 4U);
  zlib_stream.push_back(0x78U);  // CM=8, CINFO=7
  zlib_stream.push_back(0x01U);  // FCHECK makes (0x7801 % 31 == 0), no dict, fastest

  std::size_t offset = 0;
  for (std::size_t b = 0; b < block_count; ++b) {
    const std::size_t chunk_len = std::min(kMaxStoredBlock, raw.size() - offset);
    const bool is_final = (offset + chunk_len == raw.size());
    zlib_stream.push_back(is_final ? 0x01U : 0x00U);
    const auto len16 = static_cast<std::uint16_t>(chunk_len);
    const auto nlen16 = static_cast<std::uint16_t>(~len16);
    zlib_stream.push_back(static_cast<std::uint8_t>(len16 & 0xFFU));
    zlib_stream.push_back(static_cast<std::uint8_t>((len16 >> 8U) & 0xFFU));
    zlib_stream.push_back(static_cast<std::uint8_t>(nlen16 & 0xFFU));
    zlib_stream.push_back(static_cast<std::uint8_t>((nlen16 >> 8U) & 0xFFU));
    if (chunk_len > 0) {
      zlib_stream.insert(zlib_stream.end(),
                         raw.data() + offset,
                         raw.data() + offset + chunk_len);
      offset += chunk_len;
    }
  }
  detail::append_be32(zlib_stream, detail::adler32(raw.data(), raw.size()));

  std::vector<std::uint8_t> png;
  png.reserve(8U + 25U + 12U + zlib_stream.size() + 12U);
  static constexpr std::array<std::uint8_t, 8> kPngSig = {
      0x89U, 0x50U, 0x4EU, 0x47U, 0x0DU, 0x0AU, 0x1AU, 0x0AU};
  png.insert(png.end(), kPngSig.begin(), kPngSig.end());

  std::vector<std::uint8_t> ihdr;
  ihdr.reserve(13U);
  detail::append_be32(ihdr, w);
  detail::append_be32(ihdr, h);
  ihdr.push_back(8U);          // bit depth
  ihdr.push_back(color_type);  // 2 = RGB, 0 = Gray
  ihdr.push_back(0U);          // compression
  ihdr.push_back(0U);          // filter
  ihdr.push_back(0U);          // interlace
  detail::append_png_chunk(png, "IHDR", ihdr.data(), ihdr.size());
  detail::append_png_chunk(png, "IDAT", zlib_stream.data(), zlib_stream.size());
  detail::append_png_chunk(png, "IEND", nullptr, 0U);

  detail::write_bytes_atomic(out_path, png.data(), png.size());
}

ImageBuffer load_ppm(const std::filesystem::path& path) {
  std::ifstream ifs(path, std::ios::binary);
  if (!ifs.is_open()) {
    throw std::runtime_error("Failed to open PPM file: " + path.string());
  }

  const std::string magic = detail::read_ppm_token(ifs);
  if (magic != "P6" && magic != "P5") {
    throw std::runtime_error("Unsupported PPM magic '" + magic + "' in " + path.string());
  }

  const std::string w_tok = detail::read_ppm_token(ifs);
  const std::string h_tok = detail::read_ppm_token(ifs);
  int max_delim = std::char_traits<char>::eof();
  const std::string max_tok = detail::read_ppm_token(ifs, &max_delim);
  if (w_tok.empty() || h_tok.empty() || max_tok.empty()) {
    throw std::runtime_error("Truncated PPM header in " + path.string());
  }

  const std::int32_t width = detail::parse_strict_positive_int(w_tok, path);
  const std::int32_t height = detail::parse_strict_positive_int(h_tok, path);
  const std::int32_t maxval = detail::parse_strict_positive_int(max_tok, path);

  if (maxval > 255) {
    throw std::runtime_error("Out-of-range PPM maxval in " + path.string());
  }
  if (max_delim == std::char_traits<char>::eof() ||
      !std::isspace(static_cast<unsigned char>(max_delim))) {
    throw std::runtime_error("Missing whitespace after PPM maxval in " + path.string());
  }

  const PixelFormat fmt = (magic == "P6") ? PixelFormat::RGB24 : PixelFormat::Gray8;
  const std::uint64_t bpp = static_cast<std::uint64_t>(bytes_per_pixel(fmt));
  const std::uint64_t width_u64 = static_cast<std::uint64_t>(width);
  const std::uint64_t height_u64 = static_cast<std::uint64_t>(height);
  if (width_u64 > static_cast<std::uint64_t>(std::numeric_limits<std::int32_t>::max()) / bpp ||
      width_u64 > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()) /
                      (height_u64 * bpp)) {
    throw std::runtime_error("PPM dimensions overflow buffer limits in " + path.string());
  }
  const std::uint64_t expected_bytes_u64 = width_u64 * height_u64 * bpp;
  if (expected_bytes_u64 >
      static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max())) {
    throw std::runtime_error("PPM payload size exceeds stream limits in " + path.string());
  }

  auto payload_start = ifs.tellg();
  ifs.seekg(0, std::ios::end);
  const auto file_end = ifs.tellg();
  if (payload_start == std::ifstream::pos_type(-1) ||
      file_end == std::ifstream::pos_type(-1) ||
      file_end < payload_start) {
    throw std::runtime_error("Failed to determine PPM payload size in " + path.string());
  }
  auto remaining_bytes = static_cast<std::uint64_t>(file_end - payload_start);
  ifs.seekg(payload_start, std::ios::beg);
  // In Netpbm, a single whitespace byte separates maxval from raster bytes.
  // Only treat CR+LF as a 2-byte header terminator when remaining bytes exceed
  // expected_bytes_u64, so a CR delimiter followed by pixel byte 0x0A is preserved.
  if (max_delim == '\r' && remaining_bytes > expected_bytes_u64 && ifs.peek() == '\n') {
    (void)ifs.get();
    payload_start = ifs.tellg();
    --remaining_bytes;
  }
  if (remaining_bytes < expected_bytes_u64) {
    throw std::runtime_error("Truncated PPM pixel payload in " + path.string());
  }

  const auto expected_bytes = static_cast<std::size_t>(expected_bytes_u64);
  ImageBuffer buf = ImageBuffer::allocate_uninitialized(width, height, fmt);
  ifs.read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(expected_bytes));
  if (static_cast<std::size_t>(ifs.gcount()) != expected_bytes) {
    throw std::runtime_error("Truncated PPM pixel payload in " + path.string());
  }

  if (maxval != 255) {
    std::uint8_t* ptr = buf.data();
    for (std::size_t i = 0; i < expected_bytes; ++i) {
      ptr[i] = static_cast<std::uint8_t>(
          (static_cast<int>(ptr[i]) * 255) / maxval);
    }
  }
  return buf;
}

}  // namespace sublift::worker
