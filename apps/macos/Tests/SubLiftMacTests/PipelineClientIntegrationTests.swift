import AppKit
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
            let success = try client.start(pythonExecutable: pythonPath, engine: "mock")
            XCTAssertTrue(success, "握手应成功：发 hello 收 bye")
        } catch let error as PipelineClientError {
            XCTFail("PipelineClient 失败: \(error)")
        }
    }

    func testStopCleansUpResources() throws {
        let client = PipelineClient()
        try client.start(pythonExecutable: pythonPath, engine: "mock")

        let socketPath = client.socketPath
        XCTAssertFalse(socketPath.isEmpty)

        client.stop()

        XCTAssertTrue(client.socketPath.isEmpty, "stop 后 socketPath 应清空")
        XCTAssertFalse(FileManager.default.fileExists(atPath: socketPath), "stop 后 socket 文件应删除")
    }

    // MARK: - feat-016: 完整 Pipeline 跨进程测试

    /// 完整流程：start_job → frame × N → finalize → entries。
    /// 用 mock 引擎，不依赖 Vision。
    func testFullPipelineOverUDS() throws {
        let client = PipelineClient()
        defer { client.stop() }

        // start 内部已发 hello 收 bye；用 mock 引擎避免 Vision 依赖
        let success = try client.start(pythonExecutable: pythonPath, engine: "mock")
        XCTAssertTrue(success, "握手应成功")

        // 1. start_job → progress(stage=ready)
        let startJob = StartJobMessage(
            videoId: "VID-001",
            fps: 5.0,
            engine: .vision,
            confidenceThreshold: 0.5,
            regionBox: [0, 0, 320, 240],
            durationMs: 1000
        )
        let progress1 = try client.request(startJob, expecting: ProgressMessage.self)
        let p1 = try XCTUnwrap(progress1)
        XCTAssertEqual(p1.type, .progress)
        XCTAssertEqual(p1.stage, "ready")

        // 2. frame → progress(stage=processing)
        let jpegData = createTestJPEG()
        let jpegBase64 = jpegData.base64EncodedString()

        for i in 0..<5 {
            let frame = FrameMessage(
                videoId: "VID-001",
                tsMs: i * 200,
                jpegBytes: jpegBase64,
                regionBox: nil
            )
            let progress = try client.request(frame, expecting: ProgressMessage.self)
            let p = try XCTUnwrap(progress)
            XCTAssertEqual(p.stage, "processing")
        }

        // 3. finalize → entries
        let finalize = FinalizeMessage(videoId: "VID-001")
        let entries = try client.request(finalize, expecting: EntriesMessage.self)
        let e = try XCTUnwrap(entries)
        XCTAssertEqual(e.type, .entries)
        XCTAssertEqual(e.videoId, "VID-001")
    }

    /// 验证 schema 校验错误：发一条缺 video_id 的 start_job，应收到 error 响应。
    func testSchemaErrorReturnsErrorOverUDS() throws {
        let client = PipelineClient()
        defer { client.stop() }

        _ = try client.start(pythonExecutable: pythonPath, engine: "mock")

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

    // MARK: - Helpers

    /// 生成一个有效的 320x240 黑色 JPEG。
    private func createTestJPEG() -> Data {
        let size = NSSize(width: 320, height: 240)
        let image = NSImage(size: size)
        image.lockFocus()
        NSColor.black.setFill()
        NSRect(origin: .zero, size: size).fill()
        image.unlockFocus()
        let rep = NSBitmapImageRep(data: image.tiffRepresentation!)
        return rep!.representation(using: .jpeg, properties: [.compressionFactor: 0.85])!
    }
}
