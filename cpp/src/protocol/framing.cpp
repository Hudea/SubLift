#include "sublift/protocol/framing.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <cstring>
#include <unistd.h>

namespace sublift::ipc {

ssize_t read_n(int fd, void* buf, std::size_t count) noexcept {
  if (fd < 0 || buf == nullptr) {
    errno = EBADF;
    return -1;
  }
  auto* ptr = static_cast<std::uint8_t*>(buf);
  std::size_t total_read = 0;
  while (total_read < count) {
    ssize_t n = ::read(fd, ptr + total_read, count - total_read);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    if (n == 0) {
      break;
    }
    total_read += static_cast<std::size_t>(n);
  }
  return static_cast<ssize_t>(total_read);
}

ssize_t write_n(int fd, const void* buf, std::size_t count) noexcept {
  if (fd < 0 || buf == nullptr) {
    errno = EBADF;
    return -1;
  }
  const auto* ptr = static_cast<const std::uint8_t*>(buf);
  std::size_t total_written = 0;
  while (total_written < count) {
    ssize_t n = ::write(fd, ptr + total_written, count - total_written);
    if (n < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    if (n == 0) {
      errno = EIO;
      return -1;
    }
    total_written += static_cast<std::size_t>(n);
  }
  return static_cast<ssize_t>(total_written);
}

ReadFrameResult read_framed_message(int fd) {
  std::uint8_t header_buf[kLengthPrefixBytes];
  ssize_t h_read = read_n(fd, header_buf, kLengthPrefixBytes);
  if (h_read < 0) {
    return ReadFrameResult{.status = FramingStatus::IoError, .error_code = errno};
  }
  if (h_read == 0) {
    return ReadFrameResult{.status = FramingStatus::Eof};
  }
  if (static_cast<std::size_t>(h_read) < kLengthPrefixBytes) {
    return ReadFrameResult{.status = FramingStatus::IncompleteHeader};
  }

  std::uint32_t net_len = 0;
  std::memcpy(&net_len, header_buf, kLengthPrefixBytes);
  std::uint32_t payload_len = ntohl(net_len);

  if (payload_len == 0) {
    return ReadFrameResult{.status = FramingStatus::ZeroLength};
  }
  if (payload_len > kMaxMessageBytes) {
    return ReadFrameResult{.status = FramingStatus::MessageTooLarge};
  }

  ReadFrameResult res;
  res.payload.resize(payload_len);
  ssize_t b_read = read_n(fd, res.payload.data(), payload_len);
  if (b_read < 0) {
    res.status = FramingStatus::IoError;
    res.error_code = errno;
    res.payload.clear();
    return res;
  }
  if (static_cast<std::size_t>(b_read) < payload_len) {
    res.status = FramingStatus::IncompleteBody;
    res.payload.clear();
    return res;
  }

  res.status = FramingStatus::Success;
  return res;
}

FramingStatus write_framed_message(int fd, std::string_view payload) {
  if (payload.empty()) {
    return FramingStatus::ZeroLength;
  }
  if (payload.size() > kMaxMessageBytes) {
    return FramingStatus::MessageTooLarge;
  }

  std::uint32_t payload_len = static_cast<std::uint32_t>(payload.size());
  std::uint32_t net_len = htonl(payload_len);

  std::uint8_t header_buf[kLengthPrefixBytes];
  std::memcpy(header_buf, &net_len, kLengthPrefixBytes);

  if (write_n(fd, header_buf, kLengthPrefixBytes) < 0) {
    return FramingStatus::IoError;
  }

  if (write_n(fd, payload.data(), payload_len) < 0) {
    return FramingStatus::IoError;
  }

  return FramingStatus::Success;
}

}  // namespace sublift::ipc
