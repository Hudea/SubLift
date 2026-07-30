import AppKit
import XCTest
@testable import SubLiftMac

/// 跨进程集成测试：Swift PipelineClient 启动真实 Python 或 C++ Worker 子进程并握手。
///
/// 本地运行：`swift test --filter PipelineClientIntegrationTests`
final class PipelineClientIntegrationTests: XCTestCase {

    /// 复用客户端的运行时仓库定位，避免测试绑定开发者本机的绝对路径。
    private var pythonPath: String { PipelineClient.defaultPythonPath }

    private var workerPath: String? {
        try? PipelineClient.findWorkerExecutable()
    }

    func testPythonPathResolvesFromCurrentRepository() throws {
        let repoRoot = try XCTUnwrap(PipelineClient.findRepoRoot())
        let expectedPath = repoRoot.appendingPathComponent(".venv/bin/python").path

        XCTAssertEqual(pythonPath, expectedPath)
        XCTAssertTrue(
            FileManager.default.isExecutableFile(atPath: pythonPath),
            "当前仓库的 Python venv 不可执行: \(pythonPath)"
        )
    }

    func testStartAndHandshakeWithPythonServer() throws {
        let client = PipelineClient()
        defer { client.stop() }

        do {
            let success = try client.start(requestedRuntime: "python", engine: "mock", pythonExecutable: pythonPath)
            XCTAssertTrue(success, "握手应成功：发 hello 收 bye")
        } catch let error as PipelineClientError {
            XCTFail("PipelineClient 失败: \(error)")
        }
    }

    func testStopCleansUpResources() throws {
        let client = PipelineClient()
        try client.start(requestedRuntime: "python", engine: "mock", pythonExecutable: pythonPath)

        let socketPath = client.socketPath
        XCTAssertFalse(socketPath.isEmpty)

        client.stop()

        XCTAssertTrue(client.socketPath.isEmpty, "stop 后 socketPath 应清空")
        XCTAssertFalse(FileManager.default.fileExists(atPath: socketPath), "stop 后 socket 文件应删除")
    }

    // MARK: - feat-06602: C++ Worker 跨进程集成测试

    func testStartAndHandshakeWithCppWorker() throws {
        guard let workerBin = workerPath else {
            throw XCTSkip("sublift_worker 未编译于 build/cpp/bin/")
        }

        let client = PipelineClient()
        defer { client.stop() }

        let success = try client.start(
            requestedRuntime: "cpp",
            engine: "mock",
            workerExecutable: workerBin
        )
        XCTAssertTrue(success, "C++ Worker 握手应成功：发 hello 收 bye (runtime=cpp)")
    }

    func testCppWorkerFrameModeOverUDS() throws {
        guard let workerBin = workerPath else {
            throw XCTSkip("sublift_worker 未编译于 build/cpp/bin/")
        }

        let client = PipelineClient()
        defer { client.stop() }

        let success = try client.start(
            requestedRuntime: "cpp",
            engine: "mock",
            workerExecutable: workerBin
        )
        XCTAssertTrue(success)

        let startJob = StartJobMessage(
            videoId: "VID-CPP-001",
            fps: 1.0,
            engine: .mock,
            confidenceThreshold: 0.5,
            regionBox: [0, 0, 100, 100]
        )

        let progress1 = try client.request(startJob, expecting: ProgressMessage.self)
        let p1 = try XCTUnwrap(progress1)
        XCTAssertEqual(p1.stage, "ready")

        let jpegData = createTestJPEG(width: 100, height: 100)
        let jpegBase64 = jpegData.base64EncodedString()

        let frame = FrameMessage(
            videoId: "VID-CPP-001",
            tsMs: 0,
            jpegBytes: jpegBase64,
            regionBox: nil
        )
        let progress2 = try client.request(frame, expecting: ProgressMessage.self)
        let p2 = try XCTUnwrap(progress2)
        XCTAssertEqual(p2.stage, "processing")

        let finalize = FinalizeMessage(videoId: "VID-CPP-001")
        let entries = try client.request(finalize, expecting: EntriesMessage.self)
        let e = try XCTUnwrap(entries)
        XCTAssertEqual(e.videoId, "VID-CPP-001")
    }

    func testPaddleEngineFallsBackToPythonWhenCppDisabled() throws {
        let client = PipelineClient()
        defer { client.stop() }

        // 显式关闭 C++ Paddle 时，产品默认/显式 cpp 都必须可观测地 fallback。
        let success = try client.start(
            requestedRuntime: "cpp",
            engine: "paddle",
            pythonExecutable: pythonPath,
            envOverride: ["SUBLIFT_CPP_PADDLE": "0"]
        )
        XCTAssertTrue(success, "Paddle fallback 至 Python Worker 后应成功握手")
        XCTAssertEqual(
            client.lastWorkerChoice,
            WorkerChoice(runtime: .python, engine: .paddle, resolvedVia: .paddleOverride)
        )
    }

    func testPaddleProductDefaultStartsCppWhenAvailable() throws {
        let workerPath = try PipelineClient.findWorkerExecutable()
        try XCTSkipUnless(
            PipelineClient.probeCppPaddleAvailable(workerExecutable: workerPath),
            "当前 Release Worker/模型不具备 C++ Paddle，跳过 live cutover 握手"
        )

        let client = PipelineClient()
        defer { client.stop() }
        let success = try client.start(
            engine: "paddle",
            workerExecutable: workerPath
        )
        XCTAssertTrue(success, "Paddle 产品默认应以 C++ Worker 完成握手")
        XCTAssertEqual(
            client.lastWorkerChoice,
            WorkerChoice(runtime: .cpp, engine: .paddle, resolvedVia: .productDefault)
        )
    }

    // MARK: - feat-016: 完整 Pipeline 跨进程测试

    /// 完整流程：start_job → frame × N → finalize → entries。
    /// 用 mock 引擎，不依赖 Vision。
    func testFullPipelineOverUDS() throws {
        let client = PipelineClient()
        defer { client.stop() }

        // start 内部已发 hello 收 bye；用 mock 引擎避免 Vision 依赖
        let success = try client.start(engine: "mock", pythonExecutable: pythonPath)
        XCTAssertTrue(success, "握手应成功")

        // 1. start_job → progress(stage=ready)
        let startJob = StartJobMessage(
            videoId: "VID-001",
            fps: 5.0,
            engine: .mock,
            confidenceThreshold: 0.5,
            regionBox: [0, 0, 320, 240],
            durationMs: 1000
        )
        let progress1 = try client.request(startJob, expecting: ProgressMessage.self)
        let p1 = try XCTUnwrap(progress1)
        XCTAssertEqual(p1.type, .progress)
        XCTAssertEqual(p1.stage, "ready")

        // 2. frame → progress(stage=processing)
        let jpegData = createTestJPEG(width: 320, height: 240)
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
    func testInvalidStartJobReturnsError() throws {
        let client = PipelineClient()
        defer { client.stop() }

        try client.start(engine: "mock", pythonExecutable: pythonPath)

        let invalidMsg: [String: Any] = [
            "type": "start_job"
        ]

        let response = try client.request(invalidMsg)
        let dict = try XCTUnwrap(response)
        XCTAssertEqual(dict["type"] as? String, "error")
        let message = dict["message"] as? String ?? ""
        XCTAssertFalse(message.isEmpty, "error 响应应包含错误原因说明")
    }

    // MARK: - Helpers

    private func createTestJPEG(width: CGFloat = 100, height: CGFloat = 100) -> Data {
        let size = NSSize(width: width, height: height)
        let image = NSImage(size: size)
        image.lockFocus()
        NSColor.red.setFill()
        NSRect(origin: .zero, size: size).fill()
        image.unlockFocus()

        guard let tiffData = image.tiffRepresentation,
              let bitmapImage = NSBitmapImageRep(data: tiffData),
              let jpegData = bitmapImage.representation(using: .jpeg, properties: [:]) else {
            XCTFail("构造测试用 JPEG 数据失败")
            return Data()
        }
        return jpegData
    }
}
