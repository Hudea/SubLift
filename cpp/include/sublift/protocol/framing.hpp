#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <sys/types.h>

namespace sublift::ipc {

/// Maximum payload size limit per message (64 MiB).
/// Aligned with Python server.py MAX_MESSAGE_BYTES.
inline constexpr std::size_t kMaxMessageBytes = 64 * 1024 * 1024;

/// Size of the big-endian length prefix header in bytes (>I).
inline constexpr std::size_t kLengthPrefixBytes = 4;

/// Framing operation status code.
enum class FramingStatus {
  Success = 0,         ///< Frame read/written successfully
  Eof,                 ///< Peer closed connection cleanly (0 header bytes)
  ZeroLength,          ///< Header specifies length 0 (invalid frame)
  MessageTooLarge,     ///< Header length exceeds kMaxMessageBytes (invalid frame)
  IncompleteHeader,    ///< Peer closed connection before header complete
  IncompleteBody,      ///< Peer closed connection before body complete
  IoError              ///< POSIX I/O error (errno recorded)
};

/// Result of reading a framed message.
struct ReadFrameResult {
  FramingStatus status{FramingStatus::Success};
  std::string payload;
  int error_code{0};  ///< System errno if status == FramingStatus::IoError
};

/// Exact N-byte read loop with EINTR retry.
/// @return Number of bytes read. If < 0, IO error. If < count and >= 0, EOF reached.
[[nodiscard]] ssize_t read_n(int fd, void* buf, std::size_t count) noexcept;

/// Exact N-byte write loop with EINTR retry.
/// @return Number of bytes written. If < 0, IO error.
[[nodiscard]] ssize_t write_n(int fd, const void* buf, std::size_t count) noexcept;

/// Read a length-prefixed message from fd.
[[nodiscard]] ReadFrameResult read_framed_message(int fd);

/// Write a length-prefixed message to fd.
[[nodiscard]] FramingStatus write_framed_message(int fd, std::string_view payload);

}  // namespace sublift::ipc
