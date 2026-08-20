import XCTest
@testable import SubLiftMac

// MARK: - 08410 综合验收（混合目录/真实串行/资源）

@MainActor
final class Phase8AcceptanceTests: XCTestCase {

    private var tempDir: URL!
    private let fileManager = FileManager.default

    override func setUpWithError() throws {
        tempDir = FileManager.default.temporaryDirectory
            .appendingPathComponent("p8acc-\(UUID().uuidString)", isDirectory: true)
        try fileManager.createDirectory(at: tempDir, withIntermediateDirectories: true)
    }

    override func tearDownWithError() throws {
        try? fileManager.setAttributes([.posixPermissions: 0o755], ofItemAtPath: tempDir.path)
        try? fileManager.removeItem(at: tempDir)
    }

    private func write(_ name: String, in dir: URL? = nil) throws -> URL {
        let url = (dir ?? tempDir).appendingPathComponent(name)
        try Data("video bytes".utf8).write(to: url)
        return url
    }

    // MARK: - 验收 1：真实混合目录

    func testMixedFolderScanningContract() throws {
        // 结构：2 视频 + 1 无效 + 嵌套子目录（1 视频）+ 隐藏文件 + 已有 SRT。
        let a = try write("clip.mp4")
        let b = try write("movie.mov")
        _ = try write("notes.txt")
        let nested = tempDir.appendingPathComponent("nested", isDirectory: true)
        try fileManager.createDirectory(at: nested, withIntermediateDirectories: true)
        _ = try write("inner.mp4", in: nested)
        _ = try write(".hidden.mp4")
        _ = try write("clip.srt")

        // 默认非递归：只扫顶层（隐藏排除）。
        let nonRecursive = BatchInputScanner().scan(inputs: [tempDir], recursive: false)
        XCTAssertEqual(nonRecursive.accepted.count, 2, "非递归：顶层 2 视频")
        XCTAssertEqual(nonRecursive.skipped, 1, "隐藏文件计入跳过")
        XCTAssertEqual(nonRecursive.rejected.count, 2, "无效格式 notes.txt + 非视频 clip.srt 各计一次拒绝")

        // 显式递归：含嵌套视频。
        let recursive = BatchInputScanner().scan(inputs: [tempDir], recursive: true)
        XCTAssertEqual(recursive.accepted.count, 3, "递归：顶层 2 + 嵌套 1")
    }

    func testDuplicateAndExistingSRTConflictContract() throws {
        let a = try write("clip.mp4")
        _ = try write("clip.srt")

        // 批内重复去重。
        let summary = BatchInputScanner().scan(inputs: [a, a], recursive: false)
        XCTAssertEqual(summary.accepted.count, 1)
        XCTAssertEqual(summary.rejected.first?.reason, .duplicate)

        // 三冲突策略：skip 默认（已有 SRT → skipped 冲突）。
        let planner = BatchOutputPlanner()
        let plan = try planner.plan(source: a, sourceRoot: nil)
        if case .skipped = plan.conflict {
            // 默认 skip ✓
        } else {
            XCTFail("已有 SRT 默认应 skipped")
        }
        // rename：目标存在 → 稳定 (N)。创建 clip (1).srt 后 rename 到 (2)。
        _ = try write("clip (1).srt")
        let renamePlan = try BatchOutputPlanner(conflictPolicy: .rename).plan(source: a, sourceRoot: nil)
        XCTAssertEqual(renamePlan.targetURL.lastPathComponent, "clip (2).srt")
        // replace：显式确认 → 原目标。
        let replacePlan = try BatchOutputPlanner(conflictPolicy: .replace).plan(source: a, sourceRoot: nil)
        XCTAssertEqual(replacePlan.targetURL.lastPathComponent, "clip.srt")
    }

    // MARK: - 验收 2：真实串行闭环（真实 IPC，smoke 素材）

    func testSerialRunFullCycleWithRealIPC() async throws {
        guard let mp4 = spikeVideo("mp4-h264.mp4"), let mov = spikeVideo("mov-h264.mov") else {
            throw XCTSkip("素材缺失（debug/spikes/）")
        }
        // 环境 guard：worker 缺失时如实跳过（与 08206 smoke 惯例一致）。
        if (try? PipelineClient.findWorkerExecutable()) == nil {
            throw XCTSkip("sublift_worker 未编译")
        }
        // 3 任务：2 真实视频（completed）+ 1 不存在文件（failed）。
        var completedA = BatchTask.make(sourceURL: mp4, engine: .vision, quality: .fast, developerMode: false)
        var completedB = BatchTask.make(sourceURL: mov, engine: .vision, quality: .fast, developerMode: false)
        var failedTask = BatchTask.make(
            sourceURL: URL(fileURLWithPath: "/tmp/definitely-missing-\(UUID().uuidString).mp4"),
            engine: .vision, quality: .fast, developerMode: false
        )

        // 输出规划（tempDir sidecar 已存在冲突场景避免）——写到独立输出目录。
        completedA.outputURL = tempDir.appendingPathComponent("out-a.srt")
        completedB.outputURL = tempDir.appendingPathComponent("out-b.srt")
        failedTask.outputURL = tempDir.appendingPathComponent("out-fail.srt")

        let runner = BatchExtractionRunner()
        let scheduler = BatchQueueScheduler(
            runner: runner,
            repository: InMemoryBatchQueueRepository(),
            initialState: BatchQueueState(status: .idle, tasks: [completedA, completedB, failedTask], runningTaskID: nil)
        )
        scheduler.start()
        // 等待闭环完成（idle）。
        let deadline = Date().addingTimeInterval(60)
        while scheduler.state.status != .idle, Date() < deadline {
            try await Task.sleep(nanoseconds: 200_000_000)
        }
        XCTAssertEqual(scheduler.state.status, .idle, "串行闭环完成")
        XCTAssertEqual(scheduler.state.tasks[0].status, .completed)
        XCTAssertEqual(scheduler.state.tasks[1].status, .completed)
        XCTAssertEqual(scheduler.state.tasks[2].status, .failed, "失败不阻断后续项")

        // failed → retry 回 waiting（重试仍失败——文件不存在，但 retry 语义验证）。
        let retried = scheduler.retry(failedTask.id)
        XCTAssertTrue(retried)
        XCTAssertEqual(scheduler.state.tasks[2].status, .waiting)

        // 输出定位：真实产物存在（completed 原子写出）。
        XCTAssertTrue(fileManager.fileExists(atPath: completedA.outputURL!.path), "completed 输出存在")
        XCTAssertTrue(fileManager.fileExists(atPath: completedB.outputURL!.path), "completed 输出存在")
        let contentA = try Data(contentsOf: completedA.outputURL!)
        XCTAssertFalse(contentA.isEmpty, "SRT 内容非空")
    }

    /// 仓库素材定位（从测试 cwd 上溯）。
    private func spikeVideo(_ name: String) -> URL? {
        let candidates = [
            URL(fileURLWithPath: "/Volumes/lab/pp/SubLift/debug/spikes/\(name)"),
            URL(fileURLWithPath: FileManager.default.currentDirectoryPath).appendingPathComponent("debug/spikes/\(name)"),
        ]
        return candidates.first { fileManager.fileExists(atPath: $0.path) }
    }

    // MARK: - 验收 3：100 任务资源（体积/恢复时间）

    func testHundredTasksSnapshotFootprint() throws {
        var queue = BatchQueueState.empty
        for index in 0..<100 {
            var task = BatchTask.make(
                sourceURL: tempDir.appendingPathComponent("video-\(index).mp4"),
                engine: .vision, quality: .fast, developerMode: false
            )
            task.outputURL = tempDir.appendingPathComponent("video-\(index).srt")
            queue.tasks.append(task)
        }
        let repo = BatchQueueRepository(fileURL: tempDir.appendingPathComponent("queue.json"))
        try repo.save(queue)

        let size = try fileManager.attributesOfItem(atPath: tempDir.appendingPathComponent("queue.json").path)[.size] as? Int ?? 0
        let start = Date()
        let restored = try repo.load()
        let elapsed = Date().timeIntervalSince(start)

        XCTAssertEqual(restored.tasks.count, 100, "100 任务往返保真")
        XCTAssertLessThan(elapsed, 2.0, "恢复时间有界 <2s")
        print("[08410] 100-task 清单体积: \(size) 字节; 恢复时间: \(String(format: "%.3f", elapsed))s")
        XCTAssertGreaterThan(size, 0)
    }

    func testCorruptedManifestFailClosed() throws {
        let fileURL = tempDir.appendingPathComponent("queue.json")
        try Data("{ not valid json".utf8).write(to: fileURL)
        let repo = BatchQueueRepository(fileURL: fileURL)
        XCTAssertThrowsError(try repo.load()) { error in
            guard case BatchQueueRepositoryError.corrupted = error else {
                return XCTFail("应为 corrupted（结构损坏 fail-closed）")
            }
        }
        // 原文件未被修改。
        let after = try Data(contentsOf: fileURL)
        XCTAssertEqual(after, Data("{ not valid json".utf8))
    }
}
