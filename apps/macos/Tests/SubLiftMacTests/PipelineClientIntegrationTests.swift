import XCTest
@testable import SubLiftMac

/// 跨进程集成测试：Swift PipelineClient 启动真实 Python 子进程并握手。
///
/// 标记为集成测试，CI 跳过（需要 Python 环境）。
/// 本地运行：`swift test --filter PipelineClientIntegrationTests`
final class PipelineClientIntegrationTests: XCTestCase {

    /// Python 解释器路径（项目根 .venv/bin/python）。
    private let pythonPath = "/Volumes/lab/pp/SubLift/.venv/bin/python"

    func testStartAndHandshakeWithPythonServer() throws {
        let client = PipelineClient()
        defer { client.stop() }

        do {
            let success = try client.start(pythonExecutable: pythonPath)
            XCTAssertTrue(success, "握手应成功：发 hello 收 bye")
        } catch let error as PipelineClientError {
            XCTFail("PipelineClient 失败: \(error)")
        }
    }

    func testStopCleansUpResources() throws {
        let client = PipelineClient()
        try client.start(pythonExecutable: pythonPath)

        let socketPath = client.socketPath
        XCTAssertFalse(socketPath.isEmpty)

        client.stop()

        XCTAssertTrue(client.socketPath.isEmpty, "stop 后 socketPath 应清空")
        XCTAssertFalse(FileManager.default.fileExists(atPath: socketPath), "stop 后 socket 文件应删除")
    }

    // MARK: - feat-015: 7 类消息 Swift → Python 跨进程往返

    /// 在一个连接内发送 7 类消息（hello + start_job + frame + cancel_job + bye），
    /// 验证 Python server 的 stub 响应。
    /// 这是最接近真实 GUI 使用的集成测试：Swift Codable → JSON → UDS → Python JSON → handler → 响应。
    func testFullMessageSequenceOverUDS() throws {
        let client = PipelineClient()
        defer { client.stop() }

        // start 内部已发 hello 收 bye
        let success = try client.start(pythonExecutable: pythonPath)
        XCTAssertTrue(success, "握手应成功")

        // 1. start_job → progress(stage=ready)
        let startJob = StartJobMessage(
            videoId: "VID-001",
            fps: 5.0,
            engine: .vision,
            confidenceThreshold: 0.5,
            regionBox: [0, 0, 1920, 1080]
        )
        let progress1 = try client.request(startJob, expecting: ProgressMessage.self)
        let p1 = try XCTUnwrap(progress1)
        XCTAssertEqual(p1.type, .progress)
        XCTAssertEqual(p1.videoId, "VID-001")
        XCTAssertEqual(p1.stage, "ready")

        // 2. frame → progress(stage=frame_received)
        let frame = FrameMessage(
            videoId: "VID-001",
            tsMs: 1000,
            jpegBytes: "/9j/4AAQSkZJRg==",
            regionBox: nil
        )
        let progress2 = try client.request(frame, expecting: ProgressMessage.self)
        let p2 = try XCTUnwrap(progress2)
        XCTAssertEqual(p2.stage, "frame_received")
        XCTAssertEqual(p2.videoId, "VID-001")

        // 3. cancel_job → done(ok=true)
        let cancel = CancelJobMessage(videoId: "VID-001")
        let done = try client.request(cancel, expecting: DoneMessage.self)
        let d = try XCTUnwrap(done)
        XCTAssertEqual(d.type, .done)
        XCTAssertEqual(d.videoId, "VID-001")
        XCTAssertEqual(d.ok, true)
        XCTAssertNil(d.error)
    }

    /// 验证 schema 校验错误：发一条缺 video_id 的 start_job，应收到 error 响应。
    func testSchemaErrorReturnsErrorOverUDS() throws {
        let client = PipelineClient()
        defer { client.stop() }

        _ = try client.start(pythonExecutable: pythonPath)

        // 手工构造一条非法消息（缺 video_id）
        let badMessage: [String: Any] = [
            "type": "start_job",
            "fps": 5.0,
            "engine": "vision",
            "confidence_threshold": 0.5,
        ]
        let response = try client.request(badMessage)
        let resp = try XCTUnwrap(response)
        XCTAssertEqual(resp["type"] as? String, "error")
        let msg = try XCTUnwrap(resp["message"] as? String)
        XCTAssertTrue(msg.contains("video_id"), "错误消息应提及 video_id")
    }
}
