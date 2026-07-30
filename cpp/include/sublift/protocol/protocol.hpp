#pragma once

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "sublift/models.hpp"

namespace sublift::ipc {

class ProtocolError : public std::invalid_argument {
 public:
  using std::invalid_argument::invalid_argument;
};

enum class MessageType {
  Hello,
  Bye,
  StartJob,
  Frame,
  Finalize,
  CancelJob,
  Progress,
  PushEntry,
  Entries,
  Log,
  Done,
  Error,
  Unknown
};

[[nodiscard]] std::string_view to_string(MessageType type) noexcept;
[[nodiscard]] MessageType parse_message_type(std::string_view type_str) noexcept;

struct HelloMsg {
  std::string client{"sublift-mac"};
  int protocol_version{1};
};

struct ByeMsg {
  int protocol_version{1};
  std::string runtime{"cpp"};
  std::vector<std::string> engines{"mock"};
  std::vector<std::string> capabilities{"path_mode", "frame_mode", "push_entry", "cancel"};
};

struct StartJobMsg {
  std::string video_id;
  double fps{0.0};
  std::string engine;
  double confidence_threshold{0.0};
  std::optional<sublift::Box2i> region_box;
  std::int64_t duration_ms{0};
  std::optional<bool> enable_ssim_patrol;
  std::optional<std::string> video_path;
  std::optional<sublift::SubtitleProfile> subtitle_profile;
};

struct FrameMsg {
  std::string video_id;
  std::int64_t ts_ms{0};
  std::string jpeg_bytes;
  std::optional<sublift::Box2i> region_box;
};

struct FinalizeMsg {
  std::string video_id;
};

struct CancelJobMsg {
  std::string video_id;
};

struct ProgressMsg {
  std::string video_id;
  std::string stage;
  double pct{0.0};
  std::int64_t eta_ms{0};
};

struct PushEntryMsg {
  std::string video_id;
  sublift::SubtitleEntry entry;
};

struct EntriesMsg {
  std::string video_id;
  std::vector<sublift::SubtitleEntry> entries;
  std::optional<bool> is_final;
};

struct LogMsg {
  std::string video_id;
  std::string level;
  std::string msg;
};

struct DoneMsg {
  std::string video_id;
  bool ok{false};
  std::optional<std::string> error;
};

struct ErrorMsg {
  std::string message;
};

using Message = std::variant<
    HelloMsg, ByeMsg, StartJobMsg, FrameMsg, FinalizeMsg,
    CancelJobMsg, ProgressMsg, PushEntryMsg, EntriesMsg,
    LogMsg, DoneMsg, ErrorMsg
>;

/// Parse JSON string into typed Message variant. Throws ProtocolError on invalid schema/syntax.
[[nodiscard]] Message parse_message(std::string_view json_str);

/// Build canonical ByeMsg capability response.
[[nodiscard]] ByeMsg build_bye_message(
    std::vector<std::string> engines = {"mock"},
    std::vector<std::string> capabilities = {"path_mode", "frame_mode", "push_entry", "cancel"});

/// Serialize any typed Message to JSON string.
[[nodiscard]] std::string serialize_message(const Message& msg);

}  // namespace sublift::ipc
