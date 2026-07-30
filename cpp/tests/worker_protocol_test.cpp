#include <catch2/catch_test_macros.hpp>

#include "engine_factory.hpp"
#include "sublift/protocol/protocol.hpp"

using namespace sublift::ipc;

TEST_CASE("Protocol message type parsing and string conversion", "[worker][ipc][protocol]") {
  SECTION("MessageType to_string and parse_message_type round-trip") {
    std::vector<MessageType> types = {
        MessageType::Hello,   MessageType::Bye,      MessageType::StartJob,
        MessageType::Frame,   MessageType::Finalize, MessageType::CancelJob,
        MessageType::Progress, MessageType::PushEntry, MessageType::Entries,
        MessageType::Log,     MessageType::Done,     MessageType::Error,
    };

    for (auto type : types) {
      std::string_view s = to_string(type);
      REQUIRE(parse_message_type(s) == type);
    }

    REQUIRE(parse_message_type("invalid_type") == MessageType::Unknown);
  }
}

TEST_CASE("Capability ByeMsg honest representation", "[worker][ipc][protocol]") {
  ByeMsg bye = build_bye_message();
  REQUIRE(bye.protocol_version == 1);
  REQUIRE(bye.runtime == "cpp");

  // engines must contain "mock"
  REQUIRE(std::find(bye.engines.begin(), bye.engines.end(), "mock") != bye.engines.end());

  // MUST NOT report "paddle"
  REQUIRE(std::find(bye.engines.begin(), bye.engines.end(), "paddle") == bye.engines.end());

  REQUIRE(std::find(bye.capabilities.begin(), bye.capabilities.end(), "path_mode") != bye.capabilities.end());
  REQUIRE(std::find(bye.capabilities.begin(), bye.capabilities.end(), "cancel") != bye.capabilities.end());

  std::string json_str = serialize_message(bye);
  REQUIRE(json_str.find(R"("runtime":"cpp")") != std::string::npos);
  REQUIRE(json_str.find("paddle") == std::string::npos);
}

TEST_CASE("EngineFactory only advertises the engine bound to this Worker process",
          "[worker][ipc][protocol]") {
  sublift::worker::EngineFactory mock_factory("mock");
#if SUBLIFT_HAS_OPENCV
  REQUIRE(mock_factory.supported_engines() == std::vector<std::string>{"mock"});

  sublift::worker::EngineFactory vision_factory("vision");
  const auto vision_engines = vision_factory.supported_engines();
  REQUIRE((vision_engines.empty() || vision_engines == std::vector<std::string>{"vision"}));
#else
  REQUIRE(mock_factory.supported_engines().empty());
  REQUIRE(mock_factory.capabilities().empty());
  const auto error = mock_factory.validate_engine("mock");
  REQUIRE(error.has_value());
  REQUIRE(error->find("OpenCV") != std::string::npos);
#endif
}

TEST_CASE("Table-driven parsing and serialization of all 12 message types", "[worker][ipc][protocol]") {
  SECTION("HelloMsg") {
    std::string json_in = R"({"type":"hello","client":"sublift-mac","protocol_version":1})";
    Message parsed = parse_message(json_in);
    REQUIRE(std::holds_alternative<HelloMsg>(parsed));
    const auto& msg = std::get<HelloMsg>(parsed);
    REQUIRE(msg.client == "sublift-mac");
    REQUIRE(msg.protocol_version == 1);

    std::string json_out = serialize_message(parsed);
    Message roundtrip = parse_message(json_out);
    REQUIRE(std::holds_alternative<HelloMsg>(roundtrip));
  }

  SECTION("ByeMsg") {
    std::string json_in = R"({
      "type": "bye",
      "protocol_version": 1,
      "runtime": "cpp",
      "engines": ["mock"],
      "capabilities": ["path_mode", "cancel"]
    })";
    Message parsed = parse_message(json_in);
    REQUIRE(std::holds_alternative<ByeMsg>(parsed));
    const auto& msg = std::get<ByeMsg>(parsed);
    REQUIRE(msg.runtime == "cpp");

    std::string json_out = serialize_message(parsed);
    Message roundtrip = parse_message(json_out);
    REQUIRE(std::holds_alternative<ByeMsg>(roundtrip));
  }

  SECTION("StartJobMsg with path mode and subtitle profile") {
    std::string json_in = R"({
      "type": "start_job",
      "video_id": "v123",
      "fps": 2.0,
      "engine": "mock",
      "confidence_threshold": 0.6,
      "region_box": [0, 100, 1920, 200],
      "duration_ms": 10000,
      "enable_ssim_patrol": true,
      "video_path": "/path/to/video.mp4",
      "subtitle_profile": {
        "script": "cjk",
        "center_x": 960,
        "center_y": 1000,
        "height": 50,
        "y_min": 900,
        "y_max": 1050
      },
      "extra_unknown_field": "should_be_ignored"
    })";

    Message parsed = parse_message(json_in);
    REQUIRE(std::holds_alternative<StartJobMsg>(parsed));
    const auto& msg = std::get<StartJobMsg>(parsed);
    REQUIRE(msg.video_id == "v123");
    REQUIRE(msg.fps == 2.0);
    REQUIRE(msg.engine == "mock");
    REQUIRE(msg.confidence_threshold == 0.6);
    REQUIRE(msg.region_box == sublift::Box2i{0, 100, 1920, 200});
    REQUIRE(msg.video_path == "/path/to/video.mp4");
    REQUIRE(msg.subtitle_profile.has_value());
    REQUIRE(msg.subtitle_profile->script == "cjk");

    std::string json_out = serialize_message(parsed);
    Message roundtrip = parse_message(json_out);
    REQUIRE(std::holds_alternative<StartJobMsg>(roundtrip));
    const auto& rt_msg = std::get<StartJobMsg>(roundtrip);
    REQUIRE(rt_msg.video_id == "v123");
    REQUIRE(rt_msg.confidence_threshold == 0.6);
    REQUIRE(rt_msg.video_path == "/path/to/video.mp4");
  }

  SECTION("FrameMsg") {
    std::string json_in = R"({
      "type": "frame",
      "video_id": "v123",
      "ts_ms": 1000,
      "jpeg_bytes": "aGVsbG8=",
      "region_box": [0, 0, 100, 100]
    })";
    Message parsed = parse_message(json_in);
    REQUIRE(std::holds_alternative<FrameMsg>(parsed));
    const auto& msg = std::get<FrameMsg>(parsed);
    REQUIRE(msg.video_id == "v123");
    REQUIRE(msg.ts_ms == 1000);
    REQUIRE(msg.jpeg_bytes == "aGVsbG8=");
    REQUIRE(msg.region_box == sublift::Box2i{0, 0, 100, 100});

    std::string json_out = serialize_message(parsed);
    Message roundtrip = parse_message(json_out);
    REQUIRE(std::holds_alternative<FrameMsg>(roundtrip));
  }

  SECTION("FinalizeMsg & CancelJobMsg") {
    std::string fin_in = R"({"type":"finalize","video_id":"v1"})";
    Message fin = parse_message(fin_in);
    REQUIRE(std::holds_alternative<FinalizeMsg>(fin));
    REQUIRE(std::get<FinalizeMsg>(fin).video_id == "v1");
    REQUIRE(parse_message(serialize_message(fin)).index() == fin.index());

    std::string cancel_in = R"({"type":"cancel_job","video_id":"v1"})";
    Message cancel = parse_message(cancel_in);
    REQUIRE(std::holds_alternative<CancelJobMsg>(cancel));
    REQUIRE(std::get<CancelJobMsg>(cancel).video_id == "v1");
    REQUIRE(parse_message(serialize_message(cancel)).index() == cancel.index());
  }

  SECTION("ProgressMsg, PushEntryMsg, EntriesMsg, LogMsg, DoneMsg, ErrorMsg") {
    std::string prog_in = R"({"type":"progress","video_id":"v1","stage":"ocr","pct":0.5,"eta_ms":1000})";
    Message prog = parse_message(prog_in);
    REQUIRE(std::holds_alternative<ProgressMsg>(prog));
    REQUIRE(std::get<ProgressMsg>(prog).pct == 0.5);
    REQUIRE(parse_message(serialize_message(prog)).index() == prog.index());

    std::string push_in = R"({"type":"push_entry","video_id":"v1","entry":{"start_ms":100,"end_ms":200,"text":"Hello","confidence":0.95}})";
    Message push_parsed = parse_message(push_in);
    REQUIRE(std::holds_alternative<PushEntryMsg>(push_parsed));
    REQUIRE(std::get<PushEntryMsg>(push_parsed).entry.text == "Hello");
    REQUIRE(parse_message(serialize_message(push_parsed)).index() == push_parsed.index());

    std::string entries_in = R"({"type":"entries","video_id":"v1","entries":[{"start_ms":100,"end_ms":200,"text":"Line 1","confidence":0.9}],"is_final":true})";
    Message entries_parsed = parse_message(entries_in);
    REQUIRE(std::holds_alternative<EntriesMsg>(entries_parsed));
    REQUIRE(std::get<EntriesMsg>(entries_parsed).entries.size() == 1);
    REQUIRE(std::get<EntriesMsg>(entries_parsed).is_final == true);
    REQUIRE(parse_message(serialize_message(entries_parsed)).index() == entries_parsed.index());

    std::string log_in = R"({"type":"log","video_id":"v1","level":"info","msg":"test log"})";
    Message log_parsed = parse_message(log_in);
    REQUIRE(std::holds_alternative<LogMsg>(log_parsed));
    REQUIRE(std::get<LogMsg>(log_parsed).level == "info");
    REQUIRE(parse_message(serialize_message(log_parsed)).index() == log_parsed.index());

    std::string done_in = R"({"type":"done","video_id":"v1","ok":false,"error":"Engine mismatch"})";
    Message done_parsed = parse_message(done_in);
    REQUIRE(std::holds_alternative<DoneMsg>(done_parsed));
    REQUIRE(std::get<DoneMsg>(done_parsed).ok == false);
    REQUIRE(std::get<DoneMsg>(done_parsed).error == "Engine mismatch");
    REQUIRE(parse_message(serialize_message(done_parsed)).index() == done_parsed.index());

    std::string err_in = R"({"type":"error","message":"Bad payload"})";
    Message err_parsed = parse_message(err_in);
    REQUIRE(std::holds_alternative<ErrorMsg>(err_parsed));
    REQUIRE(std::get<ErrorMsg>(err_parsed).message == "Bad payload");
    REQUIRE(parse_message(serialize_message(err_parsed)).index() == err_parsed.index());
  }
}

TEST_CASE("Schema validation errors and invalid inputs", "[worker][ipc][protocol]") {
  SECTION("Invalid JSON syntax throws ProtocolError") {
    REQUIRE_THROWS_AS(parse_message("{bad_json"), ProtocolError);
  }

  SECTION("Missing required type field throws ProtocolError") {
    REQUIRE_THROWS_AS(parse_message(R"({"video_id":"v1"})"), ProtocolError);
  }

  SECTION("Missing PushEntryMsg entry object throws ProtocolError") {
    REQUIRE_THROWS_AS(parse_message(R"({"type":"push_entry","video_id":"v1"})"), ProtocolError);
  }

  SECTION("Missing StartJobMsg confidence_threshold throws ProtocolError") {
    std::string missing_conf = R"({
      "type": "start_job",
      "video_id": "v1",
      "fps": 1.0,
      "engine": "mock"
    })";
    REQUIRE_THROWS_AS(parse_message(missing_conf), ProtocolError);
  }

  SECTION("Missing ProgressMsg eta_ms throws ProtocolError") {
    std::string missing_eta = R"({
      "type": "progress",
      "video_id": "v1",
      "stage": "ocr",
      "pct": 0.5
    })";
    REQUIRE_THROWS_AS(parse_message(missing_eta), ProtocolError);
  }

  SECTION("Invalid script in SubtitleProfile throws ProtocolError") {
    std::string bad_script = R"({
      "type": "start_job",
      "video_id": "v1",
      "fps": 1.0,
      "engine": "mock",
      "confidence_threshold": 0.6,
      "subtitle_profile": {
        "script": "invalid_script_type",
        "center_x": 10,
        "center_y": 10,
        "height": 10,
        "y_min": 0,
        "y_max": 20
      }
    })";
    REQUIRE_THROWS_AS(parse_message(bad_script), ProtocolError);
  }

  SECTION("Invalid region_box array size throws ProtocolError") {
    std::string bad_box = R"({
      "type": "start_job",
      "video_id": "v1",
      "fps": 1.0,
      "engine": "mock",
      "confidence_threshold": 0.6,
      "region_box": [0, 10, 20]
    })";
    REQUIRE_THROWS_AS(parse_message(bad_box), ProtocolError);
  }

  SECTION("Invalid engine type in start_job throws ProtocolError") {
    std::string bad_engine = R"({
      "type": "start_job",
      "video_id": "v1",
      "fps": 1.0,
      "engine": "unknown_engine",
      "confidence_threshold": 0.6
    })";
    REQUIRE_THROWS_AS(parse_message(bad_engine), ProtocolError);
  }

  SECTION("Invalid log level in log throws ProtocolError") {
    std::string bad_log = R"({
      "type": "log",
      "video_id": "v1",
      "level": "verbose",
      "msg": "test"
    })";
    REQUIRE_THROWS_AS(parse_message(bad_log), ProtocolError);
  }

  SECTION("Field type mismatch throws ProtocolError") {
    std::string bad_type = R"({
      "type": "start_job",
      "video_id": "v1",
      "fps": "not_a_float",
      "engine": "mock",
      "confidence_threshold": 0.6
    })";
    REQUIRE_THROWS_AS(parse_message(bad_type), ProtocolError);
  }

  SECTION("fps and geometry fail closed") {
    REQUIRE_THROWS_AS(parse_message(R"({
      "type":"start_job", "video_id":"v1", "fps":0,
      "engine":"mock", "confidence_threshold":0.6
    })"), ProtocolError);
    REQUIRE_THROWS_AS(parse_message(R"({
      "type":"start_job", "video_id":"v1", "fps":1,
      "engine":"mock", "confidence_threshold":0.6,
      "region_box":[0,0,0,10]
    })"), ProtocolError);
    REQUIRE_THROWS_AS(parse_message(R"({
      "type":"start_job", "video_id":"v1", "fps":1,
      "engine":"mock", "confidence_threshold":0.6,
      "subtitle_profile":{"script":"cjk","center_x":1,"center_y":5,
      "height":0,"y_min":0,"y_max":10}
    })"), ProtocolError);
  }
}
