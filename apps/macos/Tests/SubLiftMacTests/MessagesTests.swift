import Foundation
import Testing
@testable import SubLiftMac

/// feat-015：IPC 消息 schema 的 Codable roundtrip 单测。
/// 验证 Swift 端 Codable struct 与 Python protocol.py 字段名一一对应。
struct MessagesTests {

    // MARK: - Request Messages (Swift → Python)

    @Test
    func startJobRoundtrip() throws {
        let profile = SubtitleProfilePayload(
            script: "cjk",
            centerX: 960,
            centerY: 40,
            height: 48,
            yMin: 10,
            yMax: 70
        )
        let msg = StartJobMessage(
            videoId: "UUID-ABCD",
            fps: 5.0,
            engine: .vision,
            confidenceThreshold: 0.5,
            regionBox: [10, 20, 100, 200],
            durationMs: 60000,
            videoPath: "/tmp/clip.mp4",
            subtitleProfile: profile
        )
        let data = try MessageCodec.encode(msg)
        let decoded = try MessageCodec.decode(data, as: StartJobMessage.self)

        #expect(decoded.videoId == "UUID-ABCD")
        #expect(decoded.fps == 5.0)
        #expect(decoded.engine == .vision)
        #expect(decoded.confidenceThreshold == 0.5)
        #expect(decoded.regionBox == [10, 20, 100, 200])
        #expect(decoded.durationMs == 60000)
        #expect(decoded.videoPath == "/tmp/clip.mp4")
        #expect(decoded.subtitleProfile == profile)
    }

    @Test
    func startJobSubtitleProfileSnakeCaseKeys() throws {
        let msg = StartJobMessage(
            videoId: "V1",
            fps: 5.0,
            engine: .vision,
            confidenceThreshold: 0.5,
            subtitleProfile: SubtitleProfilePayload(
                centerX: 1, centerY: 2, height: 3, yMin: 0, yMax: 3
            )
        )
        let data = try MessageCodec.encode(msg)
        let json = try #require(JSONSerialization.jsonObject(with: data) as? [String: Any])
        let profile = try #require(json["subtitle_profile"] as? [String: Any])
        #expect(profile["center_x"] as? Int == 1)
        #expect(profile["center_y"] as? Int == 2)
        #expect(profile["y_min"] as? Int == 0)
        #expect(profile["y_max"] as? Int == 3)
    }

    @Test
    func startJobNilRegionBox() throws {
        let msg = StartJobMessage(
            videoId: "V1",
            fps: 5.0,
            engine: .vision,
            confidenceThreshold: 0.8
        )
        let data = try MessageCodec.encode(msg)
        let decoded = try MessageCodec.decode(data, as: StartJobMessage.self)

        #expect(decoded.regionBox == nil)
        #expect(decoded.durationMs == 0)
    }

    @Test
    func startJobFieldTypeIsStartJob() throws {
        let msg = StartJobMessage(
            videoId: "V1",
            fps: 5.0,
            engine: .vision,
            confidenceThreshold: 0.5
        )
        let data = try MessageCodec.encode(msg)

        let json = try #require(JSONSerialization.jsonObject(with: data) as? [String: Any])
        #expect(json["type"] as? String == "start_job")
        #expect(json["video_id"] as? String == "V1")
        #expect(json["confidence_threshold"] as? Double == 0.5)
    }

    @Test
    func frameRoundtrip() throws {
        let msg = FrameMessage(
            videoId: "UUID-ABCD",
            tsMs: 5000,
            jpegBytes: "/9j/4AAQSkZJRg==",
            regionBox: nil
        )
        let data = try MessageCodec.encode(msg)
        let decoded = try MessageCodec.decode(data, as: FrameMessage.self)

        #expect(decoded.videoId == "UUID-ABCD")
        #expect(decoded.tsMs == 5000)
        #expect(decoded.jpegBytes == "/9j/4AAQSkZJRg==")
        #expect(decoded.regionBox == nil)
    }

    @Test
    func frameFieldTypeIsFrame() throws {
        let msg = FrameMessage(
            videoId: "V1",
            tsMs: 1000,
            jpegBytes: "AAA=",
            regionBox: nil
        )
        let data = try MessageCodec.encode(msg)

        let json = try #require(JSONSerialization.jsonObject(with: data) as? [String: Any])
        #expect(json["type"] as? String == "frame")
        #expect(json["ts_ms"] as? Int == 1000)
        #expect(json["jpeg_bytes"] as? String == "AAA=")
    }

    @Test
    func cancelJobRoundtrip() throws {
        let msg = CancelJobMessage(videoId: "UUID-ABCD")
        let data = try MessageCodec.encode(msg)
        let decoded = try MessageCodec.decode(data, as: CancelJobMessage.self)

        #expect(decoded.videoId == "UUID-ABCD")
        #expect(decoded.type == .cancelJob)
    }

    @Test
    func finalizeRoundtrip() throws {
        let msg = FinalizeMessage(videoId: "UUID-ABCD")
        let data = try MessageCodec.encode(msg)
        let decoded = try MessageCodec.decode(data, as: FinalizeMessage.self)

        #expect(decoded.videoId == "UUID-ABCD")
        #expect(decoded.type == .finalize)
    }

    @Test
    func finalizeFieldTypeIsFinalize() throws {
        let msg = FinalizeMessage(videoId: "V1")
        let data = try MessageCodec.encode(msg)

        let json = try #require(JSONSerialization.jsonObject(with: data) as? [String: Any])
        #expect(json["type"] as? String == "finalize")
        #expect(json["video_id"] as? String == "V1")
    }

    // MARK: - Response Messages (Python → Swift)

    @Test
    func progressRoundtrip() throws {
        let jsonStr = """
        {"type": "progress", "video_id": "UUID-ABCD", "stage": "ready", "pct": 0.0, "eta_ms": 0}
        """
        let data = jsonStr.data(using: .utf8)!
        let decoded = try MessageCodec.decode(data, as: ProgressMessage.self)

        #expect(decoded.type == .progress)
        #expect(decoded.videoId == "UUID-ABCD")
        #expect(decoded.stage == "ready")
        #expect(decoded.pct == 0.0)
        #expect(decoded.etaMs == 0)
    }

    @Test
    func entriesRoundtrip() throws {
        let jsonStr = """
        {
            "type": "entries",
            "video_id": "UUID-ABCD",
            "entries": [
                {"start_ms": 0, "end_ms": 1000, "text": "你好世界", "confidence": 0.95},
                {"start_ms": 2000, "end_ms": 3000, "text": "Hello World", "confidence": 0.85}
            ]
        }
        """
        let data = jsonStr.data(using: .utf8)!
        let decoded = try MessageCodec.decode(data, as: EntriesMessage.self)

        #expect(decoded.type == .entries)
        #expect(decoded.videoId == "UUID-ABCD")
        #expect(decoded.entries.count == 2)
        #expect(decoded.entries[0].startMs == 0)
        #expect(decoded.entries[0].endMs == 1000)
        #expect(decoded.entries[0].text == "你好世界")
        #expect(decoded.entries[0].confidence == 0.95)
        #expect(decoded.entries[1].text == "Hello World")
    }

    @Test
    func entriesEmptyList() throws {
        let jsonStr = """
        {"type": "entries", "video_id": "V1", "entries": []}
        """
        let data = jsonStr.data(using: .utf8)!
        let decoded = try MessageCodec.decode(data, as: EntriesMessage.self)

        #expect(decoded.entries.isEmpty)
    }

    @Test
    func logRoundtrip() throws {
        let jsonStr = """
        {"type": "log", "video_id": "UUID-ABCD", "level": "warn", "msg": "low confidence"}
        """
        let data = jsonStr.data(using: .utf8)!
        let decoded = try MessageCodec.decode(data, as: LogMessage.self)

        #expect(decoded.type == .log)
        #expect(decoded.level == .warn)
        #expect(decoded.msg == "low confidence")
    }

    @Test
    func doneRoundtripOk() throws {
        let jsonStr = """
        {"type": "done", "video_id": "UUID-ABCD", "ok": true, "error": null}
        """
        let data = jsonStr.data(using: .utf8)!
        let decoded = try MessageCodec.decode(data, as: DoneMessage.self)

        #expect(decoded.ok == true)
        #expect(decoded.error == nil)
    }

    @Test
    func doneRoundtripWithError() throws {
        let jsonStr = """
        {"type": "done", "video_id": "UUID-ABCD", "ok": false, "error": "ocr failed"}
        """
        let data = jsonStr.data(using: .utf8)!
        let decoded = try MessageCodec.decode(data, as: DoneMessage.self)

        #expect(decoded.ok == false)
        #expect(decoded.error == "ocr failed")
    }

    // MARK: - Control Messages

    @Test
    func helloRoundtrip() throws {
        let msg = HelloMessage(client: "sublift-mac")
        let data = try MessageCodec.encode(msg)
        let decoded = try MessageCodec.decode(data, as: HelloMessage.self)

        #expect(decoded.type == .hello)
        #expect(decoded.client == "sublift-mac")
    }

    @Test
    func byeRoundtrip() throws {
        let msg = ByeMessage()
        let data = try MessageCodec.encode(msg)
        let decoded = try MessageCodec.decode(data, as: ByeMessage.self)

        #expect(decoded.type == .bye)
    }

    @Test
    func errorRoundtrip() throws {
        let msg = ErrorMessage(message: "something broke")
        let data = try MessageCodec.encode(msg)
        let decoded = try MessageCodec.decode(data, as: ErrorMessage.self)

        #expect(decoded.type == .error)
        #expect(decoded.message == "something broke")
    }

    // MARK: - Enum Raw Values

    @Test
    func messageTypeRawValues() {
        #expect(MessageType.startJob.rawValue == "start_job")
        #expect(MessageType.cancelJob.rawValue == "cancel_job")
        #expect(MessageType.finalize.rawValue == "finalize")
        #expect(MessageType.hello.rawValue == "hello")
        #expect(MessageType.bye.rawValue == "bye")
        #expect(MessageType.error.rawValue == "error")
        #expect(MessageType.frame.rawValue == "frame")
        #expect(MessageType.progress.rawValue == "progress")
        #expect(MessageType.entries.rawValue == "entries")
        #expect(MessageType.log.rawValue == "log")
        #expect(MessageType.done.rawValue == "done")
    }

    @Test
    func ocrEngineRawValues() {
        #expect(OcrEngineName.vision.rawValue == "vision")
    }

    @Test
    func logLevelRawValues() {
        #expect(LogLevel.debug.rawValue == "debug")
        #expect(LogLevel.info.rawValue == "info")
        #expect(LogLevel.warn.rawValue == "warn")
        #expect(LogLevel.error.rawValue == "error")
    }
}
