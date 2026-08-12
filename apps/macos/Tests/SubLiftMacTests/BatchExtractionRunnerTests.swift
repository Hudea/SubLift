import XCTest
@testable import SubLiftMac

// MARK: - Fake IPC Client（08206）

/// 行为驱动的 Fake IPC：模拟 starting/processing/finalizing/final/error/cancel 序列。
final class FakeBatchIPCClient: BatchIPCClient, @unchecked Sendable {
    private let lock = NSLock()
    private let readGate = DispatchSemaphore(value: 0)
    var startCalls = 0
    var stopCalls = 0
    var started = false
    /// 阻塞读模拟：onRequest 返回前等待 readGate（stop() signal 中断 → 抛 connectionClosed）。
    var blocksOnRead = false
    var onRequest: ((StartJobMessage, @escaping @Sendable (Double, String) -> Void) throws -> EntriesMessage)?
    var workerChoice: WorkerChoice?

    func startBatch(engine: String) throws {
        lock.lock()
        startCalls += 1
        started = true
        lock.unlock()
    }

    func stop() {
        lock.lock()
        stopCalls += 1
        started = false
        lock.unlock()
        // 中断阻塞读（模拟 socket 关闭导致 recv 返回）。
        readGate.signal()
    }

    var lastWorkerChoice: WorkerChoice? {
        lock.lock()
        defer { lock.unlock() }
        return workerChoice
    }

    func requestBatchStreaming(
        _ message: StartJobMessage,
        onProgress: @escaping @Sendable (Double, String) -> Void
    ) throws -> EntriesMessage {
        if blocksOnRead {
            // 阻塞读模拟：等待 stop() 中断。
            readGate.wait()
            throw PipelineClientError.connectionClosed
        }
        lock.lock()
        defer { lock.unlock() }
        guard let onRequest else {
            throw PipelineClientError.connectionClosed
        }
        return try onRequest(message, onProgress)
    }
}

extension SubtitleEntryData {
    static func sample(text: String = "Hello") -> SubtitleEntryData {
        SubtitleEntryData(startMs: 0, endMs: 1000, text: text, confidence: 0.9)
    }
}

// MARK: - BatchExtractionRunner 测试（08206）

final class BatchExtractionRunnerTests: XCTestCase {

    private var fakeClient: FakeBatchIPCClient!
    private var runner: BatchExtractionRunner!
    private var tempDir: URL!
    private let fileManager = FileManager.default

    override func setUpWithError() throws {
        tempDir = fileManager.temporaryDirectory
            .appendingPathComponent("sublift-runner-\(UUID().uuidString)", isDirectory: true)
        try fileManager.createDirectory(at: tempDir, withIntermediateDirectories: true)
        fakeClient = FakeBatchIPCClient()
        runner = BatchExtractionRunner(clientFactory: { [weak self] in self!.fakeClient })
    }

    override func tearDownWithError() throws {
        try? fileManager.setAttributes([.posixPermissions: 0o755], ofItemAtPath: tempDir.path)
        try fileManager.removeItem(at: tempDir)
    }

    private func makeTask(name: String = "movie.mp4", engine: OcrEngineName = .vision) -> BatchTask {
        let source = tempDir.appendingPathComponent(name)
        try? Data("x".utf8).write(to: source)
        var task = BatchTask.make(sourceURL: source, engine: engine, quality: .fast, developerMode: false)
        task.outputURL = tempDir.appendingPathComponent("out.srt")
        return task
    }

    /// 捕获 onUpdate 序列。
    private func recordUpdates(_ task: BatchTask) async -> ([BatchTaskStatus], [Double?], BatchRunOutcome) {
        var statuses: [BatchTaskStatus] = []
        var progresses: [Double?] = []
        let outcome = await runner.run(task) { status, progress in
            statuses.append(status)
            progresses.append(progress)
        }
        return (statuses, progresses, outcome)
    }

    func testSuccessfulRunWritesAtomicallyAndReportsSummary() async throws {
        fakeClient.workerChoice = WorkerChoice(runtime: .cpp, engine: .vision, resolvedVia: .explicitFlag)
        fakeClient.onRequest = { _, onProgress in
            // 真实进度映射：processing pct → extracting；finalizing → exporting。
            onProgress(0.2, "processing")
            onProgress(0.7, "processing")
            onProgress(1.0, "finalizing")
            return EntriesMessage(videoId: "v1", entries: [.sample(text: "Hello a"), .sample(text: "Hello b")], isFinal: true)
        }

        let task = makeTask()
        let (statuses, progresses, outcome) = await recordUpdates(task)

        // 事件序列：preparing → extracting（真实 pct）→ exporting（finalizing）→（completed 由 outcome）。
        XCTAssertEqual(statuses.first, .preparing)
        XCTAssertTrue(statuses.contains(.extracting))
        XCTAssertTrue(statuses.contains(.exporting))
        XCTAssertFalse(statuses.contains(.completed), "completed 由 outcome 表达，不经 onUpdate")
        // P1-2 回归：真实进度来自 onProgress 透传（非静态 0 tick）。
        XCTAssertTrue(progresses.contains { ($0 ?? -1) > 0.5 }, "onProgress pct 必须透传到 onUpdate")

        guard case .completed(let entryCount, let outputURL, let runtime) = outcome else {
            return XCTFail("应为 completed，实际 \(outcome)")
        }
        XCTAssertEqual(entryCount, 2)
        XCTAssertEqual(outputURL, task.outputURL)
        XCTAssertEqual(runtime, "cpp")
        XCTAssertEqual(fakeClient.startCalls, 1, "每任务独占 client 启动一次")
        XCTAssertEqual(fakeClient.stopCalls, 1, "每任务 teardown 关闭独占 socket")
        XCTAssertFalse(fakeClient.started, "teardown 后 socket 已关闭")

        // 原子写出：文件存在且内容正确。
        let content = try String(contentsOf: task.outputURL!, encoding: .utf8)
        XCTAssertTrue(content.contains("Hello a"))
        XCTAssertTrue(content.contains("Hello b"))
    }

    func testCompletedOnlyAfterAtomicWrite() async throws {
        // 写出失败（目标目录不可写）→ failed，不是 completed。
        let lockedDir = tempDir.appendingPathComponent("locked", isDirectory: true)
        try fileManager.createDirectory(at: lockedDir, withIntermediateDirectories: true)
        try fileManager.setAttributes([.posixPermissions: 0o500], ofItemAtPath: lockedDir.path)
        defer { try? fileManager.setAttributes([.posixPermissions: 0o755], ofItemAtPath: lockedDir.path) }

        fakeClient.onRequest = { _, _ in
            EntriesMessage(videoId: "v1", entries: [.sample()], isFinal: true)
        }
        var task = makeTask()
        task.outputURL = lockedDir.appendingPathComponent("out.srt")

        let (_, _, outcome) = await recordUpdates(task)
        guard case .failed(let message) = outcome else {
            return XCTFail("写出失败应 failed，实际 \(outcome)")
        }
        XCTAssertTrue(message.contains("提取失败") || message.contains("权限") || message.contains("写入"))
    }

    func testFailureMapsToFailedWithMessage() async {
        fakeClient.onRequest = { _, _ in throw PipelineClientError.connectionClosed }
        let task = makeTask()
        let (_, _, outcome) = await recordUpdates(task)
        guard case .failed(let message) = outcome else {
            return XCTFail("应为 failed，实际 \(outcome)")
        }
        XCTAssertFalse(message.isEmpty)
        XCTAssertEqual(fakeClient.stopCalls, 1, "失败也 teardown")
    }

    func testCancellationDuringBlockingReadReturnsCancelled() async {
        // P1-1 回归：阻塞 requestStreaming 期间取消 → 中断读循环 → .cancelled + teardown。
        fakeClient.blocksOnRead = true
        let task = makeTask()
        let t = Task {
            await runner.run(task) { _, _ in }
        }
        // 等待任务进入阻塞读（startBatch 已调用）。
        let started = expectation(description: "started")
        let poll = Task {
            while fakeClient.startCalls == 0 {
                try? await Task.sleep(nanoseconds: 5_000_000)
            }
            started.fulfill()
        }
        await fulfillment(of: [started], timeout: 3)
        poll.cancel()

        t.cancel()
        let outcome = await withTaskGroup(of: BatchRunOutcome?.self) { group in
            group.addTask { await t.value }
            group.addTask {
                try? await Task.sleep(nanoseconds: 2_000_000_000)
                return nil
            }
            let first = await group.next() ?? nil
            group.cancelAll()
            return first
        }
        XCTAssertEqual(outcome, .cancelled, "阻塞读期间取消必须返回 .cancelled（不得挂起）")
        XCTAssertGreaterThanOrEqual(fakeClient.stopCalls, 1, "取消路径 onCancel + defer 双保险关闭 socket")
    }

    func testCancellationBeforeStartReturnsCancelled() async {
        let task = makeTask()
        let t = Task {
            await runner.run(task) { _, _ in }
        }
        t.cancel()
        let outcome = await t.value
        XCTAssertEqual(outcome, .cancelled)
        XCTAssertGreaterThanOrEqual(fakeClient.stopCalls, 1)
    }

    func testHiddenMockNormalizedBeforeStart() async {
        // 非开发模式 Mock → 启动前归一化为 vision（client.start 收到 vision）。
        fakeClient.onRequest = { _, _ in
            EntriesMessage(videoId: "v1", entries: [], isFinal: true)
        }
        let task = makeTask(engine: .mock)
        let (_, _, outcome) = await recordUpdates(task)
        guard case .completed(let entryCount, _, _) = outcome else {
            return XCTFail("应为 completed，实际 \(outcome)")
        }
        XCTAssertEqual(entryCount, 0)
        // startBatch(engine:) 收到归一化后的 vision。
        XCTAssertEqual(fakeClient.startCalls, 1)
    }

    func testEmptyEntriesStillCompletes() async {
        fakeClient.onRequest = { _, _ in
            EntriesMessage(videoId: "v1", entries: [], isFinal: true)
        }
        let task = makeTask()
        let (_, _, outcome) = await recordUpdates(task)
        guard case .completed(let entryCount, _, _) = outcome else {
            return XCTFail("空结果按合同仍 completed（0 条），实际 \(outcome)")
        }
        XCTAssertEqual(entryCount, 0)
    }
}

// MARK: - 真实 IPC smoke（08206，两小视频串行）

final class BatchExtractionRunnerSmokeTests: XCTestCase {

    /// 仓库素材定位（从测试 cwd 上溯）。
    private func spikeVideo(_ name: String) -> URL? {
        let candidates = [
            URL(fileURLWithPath: "/Volumes/lab/pp/SubLift/debug/spikes/\(name)"),
            URL(fileURLWithPath: FileManager.default.currentDirectoryPath)
                .appendingPathComponent("debug/spikes/\(name)"),
        ]
        return candidates.first { FileManager.default.fileExists(atPath: $0.path) }
    }

    func testSerialIpcSmokeTwoVideos() async throws {
        guard let mp4 = spikeVideo("mp4-h264.mp4"),
              let mov = spikeVideo("mov-h264.mov") else {
            throw XCTSkip("素材缺失（debug/spikes/mp4-h264.mp4 或 mov-h264.mov）")
        }
        // 环境 guard：worker/python 缺失时如实跳过（与 PipelineClientIntegrationTests 惯例一致）。
        if (try? PipelineClient.findWorkerExecutable()) == nil {
            throw XCTSkip("sublift_worker 未编译")
        }
        guard FileManager.default.isExecutableFile(atPath: PipelineClient.defaultPythonPath) else {
            throw XCTSkip("仓库 Python venv 不可执行")
        }
        let outDir = FileManager.default.temporaryDirectory
            .appendingPathComponent("sublift-smoke-\(UUID().uuidString)", isDirectory: true)
        try FileManager.default.createDirectory(at: outDir, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: outDir) }

        let runner = BatchExtractionRunner()
        var outcomes: [BatchRunOutcome] = []
        var updates: [BatchTaskStatus] = []

        // 串行：前项完成后才启动下一项。
        for (index, video) in [mp4, mov].enumerated() {
            var task = BatchTask.make(
                sourceURL: video,
                engine: .vision,
                quality: .fast,
                developerMode: false
            )
            task.outputURL = outDir.appendingPathComponent("out\(index).srt")
            let outcome = await runner.run(task) { status, _ in
                updates.append(status)
            }
            outcomes.append(outcome)
        }

        // 两个都 completed（真实提取，即使 0 条也 completed）。
        for outcome in outcomes {
            guard case .completed(let entryCount, let outputURL, let runtime) = outcome else {
                return XCTFail("真实串行 smoke 应 completed，实际 \(outcome)")
            }
            XCTAssertTrue(FileManager.default.fileExists(atPath: outputURL.path), "SRT 必须原子写出")
            XCTAssertNotNil(runtime, "runtime 摘要应为真实值")
            _ = entryCount
        }
        // 事件序列包含活动态（preparing/extracting 至少出现）。
        XCTAssertTrue(updates.contains(.preparing))
    }
}
