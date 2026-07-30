#include "protocol.hpp"

#include <cmath>

namespace sublift::ipc {

namespace {

std::string require_string(const nlohmann::json& j, const char* key) {
  if (!j.contains(key)) {
    throw ProtocolError(std::string("Missing required field: ") + key);
  }
  if (!j.at(key).is_string()) {
    throw ProtocolError(std::string("Field must be string: ") + key);
  }
  return j.at(key).get<std::string>();
}

int require_int(const nlohmann::json& j, const char* key) {
  if (!j.contains(key)) {
    throw ProtocolError(std::string("Missing required field: ") + key);
  }
  const auto& val = j.at(key);
  if (!val.is_number_integer() && !val.is_number_unsigned()) {
    throw ProtocolError(std::string("Field must be integer: ") + key);
  }
  return val.get<int>();
}

std::int64_t require_int64(const nlohmann::json& j, const char* key) {
  if (!j.contains(key)) {
    throw ProtocolError(std::string("Missing required field: ") + key);
  }
  const auto& val = j.at(key);
  if (!val.is_number_integer() && !val.is_number_unsigned()) {
    throw ProtocolError(std::string("Field must be integer: ") + key);
  }
  return val.get<std::int64_t>();
}

double require_double(const nlohmann::json& j, const char* key) {
  if (!j.contains(key)) {
    throw ProtocolError(std::string("Missing required field: ") + key);
  }
  const auto& val = j.at(key);
  if (!val.is_number()) {
    throw ProtocolError(std::string("Field must be number: ") + key);
  }
  return val.get<double>();
}

bool require_bool(const nlohmann::json& j, const char* key) {
  if (!j.contains(key)) {
    throw ProtocolError(std::string("Missing required field: ") + key);
  }
  if (!j.at(key).is_boolean()) {
    throw ProtocolError(std::string("Field must be boolean: ") + key);
  }
  return j.at(key).get<bool>();
}

sublift::Box2i parse_box2i(const nlohmann::json& j) {
  if (!j.is_array() || j.size() != 4) {
    throw ProtocolError("region_box must be array of 4 integers");
  }
  for (const auto& elem : j) {
    if (!elem.is_number_integer() && !elem.is_number_unsigned()) {
      throw ProtocolError("region_box elements must be integers");
    }
  }
  sublift::Box2i box{
      .x = j[0].get<std::int32_t>(),
      .y = j[1].get<std::int32_t>(),
      .width = j[2].get<std::int32_t>(),
      .height = j[3].get<std::int32_t>(),
  };
  if (box.x < 0 || box.y < 0 || box.width <= 0 || box.height <= 0) {
    throw ProtocolError("region_box must have non-negative origin and positive dimensions");
  }
  return box;
}

nlohmann::json serialize_box2i(const sublift::Box2i& box) {
  return nlohmann::json::array({box.x, box.y, box.width, box.height});
}

sublift::SubtitleEntry parse_entry(const nlohmann::json& j) {
  if (!j.is_object()) {
    throw ProtocolError("SubtitleEntry must be an object");
  }
  sublift::SubtitleEntry e;
  e.start_ms = require_int64(j, "start_ms");
  e.end_ms = require_int64(j, "end_ms");
  e.text = require_string(j, "text");
  e.confidence = require_double(j, "confidence");
  return e;
}

nlohmann::json serialize_entry(const sublift::SubtitleEntry& e) {
  return nlohmann::json{
      {"start_ms", e.start_ms},
      {"end_ms", e.end_ms},
      {"text", e.text},
      {"confidence", e.confidence},
  };
}

sublift::SubtitleProfile parse_profile(const nlohmann::json& j) {
  if (!j.is_object()) {
    throw ProtocolError("SubtitleProfile must be an object");
  }
  sublift::SubtitleProfile p;
  p.script = require_string(j, "script");
  if (!sublift::is_valid_script(p.script)) {
    throw ProtocolError("Invalid script in SubtitleProfile: " + p.script);
  }
  p.center_x = require_int(j, "center_x");
  p.center_y = require_int(j, "center_y");
  p.height = require_int(j, "height");
  p.y_min = require_int(j, "y_min");
  p.y_max = require_int(j, "y_max");
  const bool script_only = p.center_x == 0 && p.center_y == 0 && p.height == 0 &&
      p.y_min == 0 && p.y_max == 0;
  if (!script_only && (p.center_x < 0 || p.center_y < 0 || p.height <= 0 ||
                       p.y_min < 0 || p.y_max < p.y_min || p.center_y < p.y_min ||
                       p.center_y > p.y_max)) {
    throw ProtocolError("SubtitleProfile geometry is invalid");
  }
  return p;
}

nlohmann::json serialize_profile(const sublift::SubtitleProfile& p) {
  return nlohmann::json{
      {"script", p.script},
      {"center_x", p.center_x},
      {"center_y", p.center_y},
      {"height", p.height},
      {"y_min", p.y_min},
      {"y_max", p.y_max},
  };
}

}  // namespace

std::string_view to_string(MessageType type) noexcept {
  switch (type) {
    case MessageType::Hello:
      return "hello";
    case MessageType::Bye:
      return "bye";
    case MessageType::StartJob:
      return "start_job";
    case MessageType::Frame:
      return "frame";
    case MessageType::Finalize:
      return "finalize";
    case MessageType::CancelJob:
      return "cancel_job";
    case MessageType::Progress:
      return "progress";
    case MessageType::PushEntry:
      return "push_entry";
    case MessageType::Entries:
      return "entries";
    case MessageType::Log:
      return "log";
    case MessageType::Done:
      return "done";
    case MessageType::Error:
      return "error";
    default:
      return "unknown";
  }
}

MessageType parse_message_type(std::string_view type_str) noexcept {
  if (type_str == "hello") return MessageType::Hello;
  if (type_str == "bye") return MessageType::Bye;
  if (type_str == "start_job") return MessageType::StartJob;
  if (type_str == "frame") return MessageType::Frame;
  if (type_str == "finalize") return MessageType::Finalize;
  if (type_str == "cancel_job") return MessageType::CancelJob;
  if (type_str == "progress") return MessageType::Progress;
  if (type_str == "push_entry") return MessageType::PushEntry;
  if (type_str == "entries") return MessageType::Entries;
  if (type_str == "log") return MessageType::Log;
  if (type_str == "done") return MessageType::Done;
  if (type_str == "error") return MessageType::Error;
  return MessageType::Unknown;
}

ByeMsg build_bye_message(std::vector<std::string> engines,
                       std::vector<std::string> capabilities) {
  ByeMsg bye;
  bye.protocol_version = 1;
  bye.runtime = "cpp";
  bye.engines = std::move(engines);
  bye.capabilities = std::move(capabilities);
  return bye;
}

Message parse_message(std::string_view json_str) {
  nlohmann::json root;
  try {
    root = nlohmann::json::parse(json_str);
  } catch (const std::exception& e) {
    throw ProtocolError(std::string("JSON parse error: ") + e.what());
  }

  if (!root.is_object()) {
    throw ProtocolError("JSON root must be an object");
  }

  std::string type_str = require_string(root, "type");
  MessageType type = parse_message_type(type_str);

  switch (type) {
    case MessageType::Hello: {
      HelloMsg msg;
      if (root.contains("client") && !root.at("client").is_null()) {
        msg.client = require_string(root, "client");
      }
      if (root.contains("protocol_version") && !root.at("protocol_version").is_null()) {
        msg.protocol_version = require_int(root, "protocol_version");
      }
      return msg;
    }
    case MessageType::Bye: {
      ByeMsg msg;
      if (root.contains("protocol_version") && !root.at("protocol_version").is_null()) {
        msg.protocol_version = require_int(root, "protocol_version");
      }
      if (root.contains("runtime") && !root.at("runtime").is_null()) {
        msg.runtime = require_string(root, "runtime");
      }
      if (root.contains("engines") && root.at("engines").is_array()) {
        msg.engines.clear();
        for (const auto& e : root.at("engines")) {
          if (e.is_string()) msg.engines.push_back(e.get<std::string>());
        }
      }
      if (root.contains("capabilities") && root.at("capabilities").is_array()) {
        msg.capabilities.clear();
        for (const auto& c : root.at("capabilities")) {
          if (c.is_string()) msg.capabilities.push_back(c.get<std::string>());
        }
      }
      return msg;
    }
    case MessageType::StartJob: {
      StartJobMsg msg;
      msg.video_id = require_string(root, "video_id");
      msg.fps = require_double(root, "fps");
      if (!std::isfinite(msg.fps) || msg.fps <= 0.0) {
        throw ProtocolError("fps must be finite and > 0");
      }
      msg.engine = require_string(root, "engine");
      if (msg.engine != "mock" && msg.engine != "vision" && msg.engine != "paddle") {
        throw ProtocolError("Invalid engine type: " + msg.engine);
      }
      msg.confidence_threshold = require_double(root, "confidence_threshold");
      if (root.contains("region_box") && !root.at("region_box").is_null()) {
        msg.region_box = parse_box2i(root.at("region_box"));
      }
      if (root.contains("duration_ms") && !root.at("duration_ms").is_null()) {
        msg.duration_ms = require_int64(root, "duration_ms");
      }
      if (root.contains("enable_ssim_patrol") && !root.at("enable_ssim_patrol").is_null()) {
        msg.enable_ssim_patrol = require_bool(root, "enable_ssim_patrol");
      }
      if (root.contains("video_path") && !root.at("video_path").is_null()) {
        msg.video_path = require_string(root, "video_path");
      }
      if (root.contains("subtitle_profile") && !root.at("subtitle_profile").is_null()) {
        msg.subtitle_profile = parse_profile(root.at("subtitle_profile"));
      }
      return msg;
    }
    case MessageType::Frame: {
      FrameMsg msg;
      msg.video_id = require_string(root, "video_id");
      msg.ts_ms = require_int64(root, "ts_ms");
      msg.jpeg_bytes = require_string(root, "jpeg_bytes");
      if (root.contains("region_box") && !root.at("region_box").is_null()) {
        msg.region_box = parse_box2i(root.at("region_box"));
      }
      return msg;
    }
    case MessageType::Finalize: {
      FinalizeMsg msg;
      msg.video_id = require_string(root, "video_id");
      return msg;
    }
    case MessageType::CancelJob: {
      CancelJobMsg msg;
      msg.video_id = require_string(root, "video_id");
      return msg;
    }
    case MessageType::Progress: {
      ProgressMsg msg;
      msg.video_id = require_string(root, "video_id");
      msg.stage = require_string(root, "stage");
      msg.pct = require_double(root, "pct");
      msg.eta_ms = require_int64(root, "eta_ms");
      return msg;
    }
    case MessageType::PushEntry: {
      PushEntryMsg msg;
      msg.video_id = require_string(root, "video_id");
      if (!root.contains("entry") || root.at("entry").is_null()) {
        throw ProtocolError("Missing required field: entry");
      }
      msg.entry = parse_entry(root.at("entry"));
      return msg;
    }
    case MessageType::Entries: {
      EntriesMsg msg;
      msg.video_id = require_string(root, "video_id");
      if (!root.contains("entries") || !root.at("entries").is_array()) {
        throw ProtocolError("EntriesMsg must contain entries array");
      }
      for (const auto& item : root.at("entries")) {
        msg.entries.push_back(parse_entry(item));
      }
      if (root.contains("is_final") && !root.at("is_final").is_null()) {
        msg.is_final = require_bool(root, "is_final");
      }
      return msg;
    }
    case MessageType::Log: {
      LogMsg msg;
      msg.video_id = require_string(root, "video_id");
      msg.level = require_string(root, "level");
      if (msg.level != "debug" && msg.level != "info" && msg.level != "warn" && msg.level != "error") {
        throw ProtocolError("Invalid log level: " + msg.level);
      }
      msg.msg = require_string(root, "msg");
      return msg;
    }
    case MessageType::Done: {
      DoneMsg msg;
      msg.video_id = require_string(root, "video_id");
      msg.ok = require_bool(root, "ok");
      if (root.contains("error") && !root.at("error").is_null()) {
        msg.error = require_string(root, "error");
      }
      return msg;
    }
    case MessageType::Error: {
      ErrorMsg msg;
      msg.message = require_string(root, "message");
      return msg;
    }
    default:
      throw ProtocolError("Unknown or unsupported message type: " + type_str);
  }
}

std::string serialize_message(const Message& msg) {
  nlohmann::json j;

  std::visit(
      [&j](auto&& arg) {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, HelloMsg>) {
          j["type"] = "hello";
          j["client"] = arg.client;
          j["protocol_version"] = arg.protocol_version;
        } else if constexpr (std::is_same_v<T, ByeMsg>) {
          j["type"] = "bye";
          j["protocol_version"] = arg.protocol_version;
          j["runtime"] = arg.runtime;
          j["engines"] = arg.engines;
          j["capabilities"] = arg.capabilities;
        } else if constexpr (std::is_same_v<T, StartJobMsg>) {
          j["type"] = "start_job";
          j["video_id"] = arg.video_id;
          j["fps"] = arg.fps;
          j["engine"] = arg.engine;
          j["confidence_threshold"] = arg.confidence_threshold;
          if (arg.region_box) j["region_box"] = serialize_box2i(*arg.region_box);
          j["duration_ms"] = arg.duration_ms;
          if (arg.enable_ssim_patrol) j["enable_ssim_patrol"] = *arg.enable_ssim_patrol;
          if (arg.video_path) j["video_path"] = *arg.video_path;
          if (arg.subtitle_profile) j["subtitle_profile"] = serialize_profile(*arg.subtitle_profile);
        } else if constexpr (std::is_same_v<T, FrameMsg>) {
          j["type"] = "frame";
          j["video_id"] = arg.video_id;
          j["ts_ms"] = arg.ts_ms;
          j["jpeg_bytes"] = arg.jpeg_bytes;
          if (arg.region_box) j["region_box"] = serialize_box2i(*arg.region_box);
        } else if constexpr (std::is_same_v<T, FinalizeMsg>) {
          j["type"] = "finalize";
          j["video_id"] = arg.video_id;
        } else if constexpr (std::is_same_v<T, CancelJobMsg>) {
          j["type"] = "cancel_job";
          j["video_id"] = arg.video_id;
        } else if constexpr (std::is_same_v<T, ProgressMsg>) {
          j["type"] = "progress";
          j["video_id"] = arg.video_id;
          j["stage"] = arg.stage;
          j["pct"] = arg.pct;
          j["eta_ms"] = arg.eta_ms;
        } else if constexpr (std::is_same_v<T, PushEntryMsg>) {
          j["type"] = "push_entry";
          j["video_id"] = arg.video_id;
          j["entry"] = serialize_entry(arg.entry);
        } else if constexpr (std::is_same_v<T, EntriesMsg>) {
          j["type"] = "entries";
          j["video_id"] = arg.video_id;
          nlohmann::json arr = nlohmann::json::array();
          for (const auto& e : arg.entries) {
            arr.push_back(serialize_entry(e));
          }
          j["entries"] = arr;
          if (arg.is_final) j["is_final"] = *arg.is_final;
        } else if constexpr (std::is_same_v<T, LogMsg>) {
          j["type"] = "log";
          j["video_id"] = arg.video_id;
          j["level"] = arg.level;
          j["msg"] = arg.msg;
        } else if constexpr (std::is_same_v<T, DoneMsg>) {
          j["type"] = "done";
          j["video_id"] = arg.video_id;
          j["ok"] = arg.ok;
          if (arg.error) j["error"] = *arg.error;
        } else if constexpr (std::is_same_v<T, ErrorMsg>) {
          j["type"] = "error";
          j["message"] = arg.message;
        }
      },
      msg);

  return j.dump();
}

}  // namespace sublift::ipc
