#include "hash.hpp"

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

namespace sublift::adapters::paddle::detail {
namespace {

constexpr std::uint32_t rotr(std::uint32_t x, std::uint32_t n) noexcept {
  return (x >> n) | (x << (32 - n));
}

void sha256_compress(std::uint32_t state[8], const std::uint8_t block[64]) {
  static constexpr std::uint32_t k[64] = {
      0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu,
      0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u, 0xd807aa98u, 0x12835b01u,
      0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u,
      0xc19bf174u, 0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
      0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau, 0x983e5152u,
      0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u,
      0x06ca6351u, 0x14292967u, 0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu,
      0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
      0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u,
      0xd6990624u, 0xf40e3585u, 0x106aa070u, 0x19a4c116u, 0x1e376c08u,
      0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu,
      0x682e6ff3u, 0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
      0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
  };

  std::uint32_t w[64];
  for (int i = 0; i < 16; ++i) {
    w[i] = (static_cast<std::uint32_t>(block[i * 4]) << 24) |
           (static_cast<std::uint32_t>(block[i * 4 + 1]) << 16) |
           (static_cast<std::uint32_t>(block[i * 4 + 2]) << 8) |
           static_cast<std::uint32_t>(block[i * 4 + 3]);
  }
  for (int i = 16; i < 64; ++i) {
    const std::uint32_t s0 =
        rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
    const std::uint32_t s1 =
        rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
    w[i] = w[i - 16] + s0 + w[i - 7] + s1;
  }

  std::uint32_t a = state[0];
  std::uint32_t b = state[1];
  std::uint32_t c = state[2];
  std::uint32_t d = state[3];
  std::uint32_t e = state[4];
  std::uint32_t f = state[5];
  std::uint32_t g = state[6];
  std::uint32_t h = state[7];

  for (int i = 0; i < 64; ++i) {
    const std::uint32_t sum1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
    const std::uint32_t choice = (e & f) ^ ((~e) & g);
    const std::uint32_t temp1 = h + sum1 + choice + k[i] + w[i];
    const std::uint32_t sum0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
    const std::uint32_t majority = (a & b) ^ (a & c) ^ (b & c);
    const std::uint32_t temp2 = sum0 + majority;
    h = g;
    g = f;
    f = e;
    e = d + temp1;
    d = c;
    c = b;
    b = a;
    a = temp1 + temp2;
  }

  state[0] += a;
  state[1] += b;
  state[2] += c;
  state[3] += d;
  state[4] += e;
  state[5] += f;
  state[6] += g;
  state[7] += h;
}

class Sha256State {
 public:
  Sha256State() = default;

  void update(const std::uint8_t* data, std::size_t size) {
    if (data == nullptr || size == 0) {
      return;
    }
    total_bytes_ += static_cast<std::uint64_t>(size);
    std::size_t offset = 0;

    if (buffer_len_ > 0) {
      const std::size_t need = 64 - buffer_len_;
      const std::size_t copy_len = size < need ? size : need;
      std::memcpy(buffer_.data() + buffer_len_, data, copy_len);
      buffer_len_ += copy_len;
      offset += copy_len;
      if (buffer_len_ == 64) {
        sha256_compress(state_, buffer_.data());
        buffer_len_ = 0;
      }
    }

    while (offset + 64 <= size) {
      sha256_compress(state_, data + offset);
      offset += 64;
    }

    const std::size_t remaining = size - offset;
    if (remaining > 0) {
      std::memcpy(buffer_.data(), data + offset, remaining);
      buffer_len_ = remaining;
    }
  }

  std::array<std::uint8_t, 32> finalize() {
    std::array<std::uint8_t, 64> block{};
    if (buffer_len_ > 0) {
      std::memcpy(block.data(), buffer_.data(), buffer_len_);
    }
    block[buffer_len_] = 0x80;
    if (buffer_len_ + 1 <= 56) {
      std::memset(block.data() + buffer_len_ + 1, 0, 56 - buffer_len_ - 1);
    } else {
      std::memset(block.data() + buffer_len_ + 1, 0, 64 - buffer_len_ - 1);
      sha256_compress(state_, block.data());
      std::memset(block.data(), 0, 56);
    }

    const std::uint64_t bit_size = total_bytes_ * 8ULL;
    for (int i = 0; i < 8; ++i) {
      block[63 - i] =
          static_cast<std::uint8_t>((bit_size >> (8 * i)) & 0xffU);
    }
    sha256_compress(state_, block.data());

    std::array<std::uint8_t, 32> result{};
    for (int i = 0; i < 8; ++i) {
      result[static_cast<std::size_t>(i) * 4] =
          static_cast<std::uint8_t>((state_[i] >> 24) & 0xffU);
      result[static_cast<std::size_t>(i) * 4 + 1] =
          static_cast<std::uint8_t>((state_[i] >> 16) & 0xffU);
      result[static_cast<std::size_t>(i) * 4 + 2] =
          static_cast<std::uint8_t>((state_[i] >> 8) & 0xffU);
      result[static_cast<std::size_t>(i) * 4 + 3] =
          static_cast<std::uint8_t>(state_[i] & 0xffU);
    }
    return result;
  }

 private:
  std::uint32_t state_[8] = {
      0x6a09e667u, 0xbb67ae85u, 0x3c6ef372u, 0xa54ff53au,
      0x510e527fu, 0x9b05688cu, 0x1f83d9abu, 0x5be0cd19u,
  };
  std::array<std::uint8_t, 64> buffer_{};
  std::size_t buffer_len_{0};
  std::uint64_t total_bytes_{0};
};

std::string digest_to_hex(const std::array<std::uint8_t, 32>& digest) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string result;
  result.reserve(64);
  for (const auto byte : digest) {
    result.push_back(kHex[(byte >> 4) & 0x0f]);
    result.push_back(kHex[byte & 0x0f]);
  }
  return result;
}

struct FileIdentity {
  std::string normalized_path;
  std::uint64_t dev{0};
  std::uint64_t ino{0};
  std::uint64_t size{0};
  std::int64_t mtime_sec{0};
  std::int64_t mtime_nsec{0};

  bool operator==(const FileIdentity& other) const = default;
};

struct FileIdentityHash {
  std::size_t operator()(const FileIdentity& id) const noexcept {
    std::size_t h = std::hash<std::string>{}(id.normalized_path);
    auto mix = [&h](std::size_t v) {
      h ^= v + 0x9e3779b97f4a7c15ULL + (h << 6) + (h >> 2);
    };
    mix(std::hash<std::uint64_t>{}(id.dev));
    mix(std::hash<std::uint64_t>{}(id.ino));
    mix(std::hash<std::uint64_t>{}(id.size));
    mix(std::hash<std::int64_t>{}(id.mtime_sec));
    mix(std::hash<std::int64_t>{}(id.mtime_nsec));
    return h;
  }
};

std::optional<FileIdentity> query_file_identity(const std::filesystem::path& path) {
  std::error_code ec;
  const auto canonical = std::filesystem::weakly_canonical(path, ec);
  std::string norm_path =
      ec ? path.lexically_normal().string() : canonical.string();

#if !defined(_WIN32)
  struct ::stat st{};
  if (::stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) {
    return std::nullopt;
  }
  FileIdentity id;
  id.normalized_path = std::move(norm_path);
  id.dev = static_cast<std::uint64_t>(st.st_dev);
  id.ino = static_cast<std::uint64_t>(st.st_ino);
  id.size = static_cast<std::uint64_t>(st.st_size);
#if defined(__APPLE__)
  id.mtime_sec = static_cast<std::int64_t>(st.st_mtimespec.tv_sec);
  id.mtime_nsec = static_cast<std::int64_t>(st.st_mtimespec.tv_nsec);
#else
  id.mtime_sec = static_cast<std::int64_t>(st.st_mtim.tv_sec);
  id.mtime_nsec = static_cast<std::int64_t>(st.st_mtim.tv_nsec);
#endif
  return id;
#else
  if (!std::filesystem::is_regular_file(path, ec) || ec) {
    return std::nullopt;
  }
  const auto size = std::filesystem::file_size(path, ec);
  if (ec) {
    return std::nullopt;
  }
  const auto mtime = std::filesystem::last_write_time(path, ec);
  if (ec) {
    return std::nullopt;
  }
  FileIdentity id;
  id.normalized_path = std::move(norm_path);
  id.size = static_cast<std::uint64_t>(size);
  id.mtime_sec = static_cast<std::int64_t>(mtime.time_since_epoch().count());
  return id;
#endif
}

std::mutex& file_hash_cache_mutex() {
  static std::mutex mu;
  return mu;
}

std::unordered_map<FileIdentity, std::string, FileIdentityHash>& file_hash_cache() {
  static std::unordered_map<FileIdentity, std::string, FileIdentityHash> cache;
  return cache;
}

}  // namespace

std::string sha256_hex(const std::uint8_t* data, std::size_t size) {
  if (data == nullptr && size != 0) {
    return std::string(64, '0');
  }
  Sha256State state;
  if (data != nullptr && size > 0) {
    state.update(data, size);
  }
  return digest_to_hex(state.finalize());
}

std::string sha256_prefixed(const std::uint8_t* data, std::size_t size) {
  return "sha256:" + sha256_hex(data, size);
}

std::optional<std::string> sha256_file_hex(const std::filesystem::path& path) {
  const auto id = query_file_identity(path);
  if (id.has_value()) {
    std::lock_guard<std::mutex> lock(file_hash_cache_mutex());
    auto& cache = file_hash_cache();
    if (const auto it = cache.find(*id); it != cache.end()) {
      return it->second;
    }
  }

  std::ifstream in(path, std::ios::binary);
  if (!in) return std::nullopt;

  Sha256State state;
  std::array<char, 65536> buf{};
  while (in.read(buf.data(), static_cast<std::streamsize>(buf.size())) ||
         in.gcount() > 0) {
    state.update(reinterpret_cast<const std::uint8_t*>(buf.data()),
                 static_cast<std::size_t>(in.gcount()));
  }
  if (in.bad()) return std::nullopt;

  std::string hex = digest_to_hex(state.finalize());
  if (id.has_value()) {
    std::lock_guard<std::mutex> lock(file_hash_cache_mutex());
    file_hash_cache().emplace(*id, hex);
  }
  return hex;
}

}  // namespace sublift::adapters::paddle::detail
