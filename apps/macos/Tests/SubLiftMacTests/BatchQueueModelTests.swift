import XCTest
@testable import SubLiftMac

// MARK: - 批量队列模型交互（08309）

@MainActor
final class BatchQueueModelTests: XCTestCase {

    private var tempDir: URL!
    private var fileManager = FileManager.default

    override func setUpWithError() throws {
        tempDir = FileManager.default.temporaryDirectory
            .appendingPathComponent("bqm-\(UUID().uuidString)", isDirectory: true)
        try fileManager.createDirectory(at: tempDir, withIntermediateDirectories: true)
    }

    override func tearDownWithError() throws {
        try? fileManager.setAttributes([.posixPermissions: 0o755], ofItemAtPath: tempDir.path)
        try? fileManager.removeItem(at: tempDir)
    }

    private func makeModel(existing: Set<URL> = []) -> BatchQueueModel {
        let repository = InMemoryBatchQueueRepository()
        return BatchQueueModel(runner: FakeBatchRunner(), repository: repository)
    }

    private func makeVideo(_ name: String) throws -> URL {
        let url = tempDir.appendingPathComponent(name)
        try Data("video".utf8).write(to: url)
        return url
    }

    private func makeSubdir(_ name: String) throws -> URL {
        let url = tempDir.appendingPathComponent(name, isDirectory: true)
        try fileManager.createDirectory(at: url, withIntermediateDirectories: true)
        return url
    }

    // MARK: - 统一导入（scanner 接线）

    func testImportInputsScansAndAddsAccepted() throws {
        let a = try makeVideo("a.mp4")
        let b = try makeVideo("b.mov")
        let unsupported = try makeVideo("c.txt")
        let model = makeModel()
        model.importInputs([a, b, unsupported])

        XCTAssertEqual(model.state.tasks.count, 2, "仅接受支持的视频")
        XCTAssertEqual(model.lastScanSummary?.accepted.count, 2)
        XCTAssertEqual(model.lastScanSummary?.rejected.count, 1)
        if case .unsupportedFormat = model.lastScanSummary?.rejected.first?.reason {
            // 结构化拒绝原因（unsupportedFormat 带扩展名）。
        } else {
            XCTFail("应为 unsupportedFormat 结构化拒绝")
        }
        // B02：不自动开始。
        XCTAssertEqual(model.state.status, .idle)
    }

    func testImportFolderScansDirectory() throws {
        let sub = try makeSubdir("videos")
        try Data("video".utf8).write(to: tempDir.appendingPathComponent("videos/in1.mp4"))
        let outer = try makeVideo("outer.mp4")
        let model = makeModel()
        model.importInputs([sub, outer])

        XCTAssertEqual(model.state.tasks.count, 2)
    }

    func testImportDuplicateWithinBatchRejected() throws {
        let a = try makeVideo("a.mp4")
        let model = makeModel()
        model.importInputs([a, a])
        XCTAssertEqual(model.state.tasks.count, 1, "批内去重")
        XCTAssertEqual(model.lastScanSummary?.rejected.first?.reason, .duplicate)
    }

    func testImportDuplicateAgainstExistingQueueRejected() throws {
        let a = try makeVideo("a.mp4")
        let model = makeModel()
        model.importInputs([a])
        model.importInputs([a])
        XCTAssertEqual(model.state.tasks.count, 1, "与已有队列去重")
    }

    // MARK: - 投影（搜索/筛选只改变投影）

    func testSearchFilterOnlyChangesProjection() throws {
        let a = try makeVideo("interview.mp4")
        let b = try makeVideo("lecture.mov")
        let model = makeModel()
        model.importInputs([a, b])
        model.searchText = "interview"
        XCTAssertEqual(model.filteredTasks.count, 1)
        XCTAssertEqual(model.filteredTasks.first?.sourceURL.lastPathComponent, "interview.mp4")
        XCTAssertEqual(model.state.tasks.count, 2, "投影不改变真实队列")
        model.searchText = ""
        XCTAssertEqual(model.filteredTasks.count, 2)
    }

    func testStatusFilterProjection() throws {
        let a = try makeVideo("a.mp4")
        let model = makeModel()
        model.importInputs([a])
        // 模拟任务进入终态（直接构造状态）。
        model.scheduler.cancel(model.state.tasks[0].id)
        model.statusFilter = .cancelled
        XCTAssertEqual(model.filteredTasks.count, 1)
        model.statusFilter = .waiting
        XCTAssertEqual(model.filteredTasks.count, 0)
    }

    // MARK: - 输出规划（开始前集中确认）

    func testPrepareOutputPlanDetectsConflicts() throws {
        let a = try makeVideo("a.mp4")
        // 已存在同名 SRT。
        try Data("existing".utf8).write(to: tempDir.appendingPathComponent("a.srt"))
        let model = makeModel()
        model.importInputs([a])
        model.prepareOutputPlan()

        let conflicts = model.outputConflicts
        XCTAssertEqual(conflicts.count, 1)
        XCTAssertEqual(conflicts.first?.outputURL.lastPathComponent, "a.srt")
    }

    func testConfirmReplaceReplansAndStarts() throws {
        let a = try makeVideo("a.mp4")
        try Data("existing".utf8).write(to: tempDir.appendingPathComponent("a.srt"))
        let model = makeModel()
        model.importInputs([a])
        // 无冲突任务先规划 outputURL。
        model.prepareOutputPlan()
        XCTAssertFalse(model.outputConflicts.isEmpty)
        // 确认替换 → 冲突任务以 replace 策略写入 outputURL + 启动。
        model.confirmOutputConflictsAndStart()
        XCTAssertEqual(model.state.tasks.first?.outputURL?.lastPathComponent, "a.srt")
        XCTAssertEqual(model.state.status, .running)
    }

    func testCancelStartDoesNotTouchOutputs() throws {
        let a = try makeVideo("a.mp4")
        let existingSRT = tempDir.appendingPathComponent("a.srt")
        try Data("existing srt bytes".utf8).write(to: existingSRT)
        let model = makeModel()
        model.importInputs([a])
        model.prepareOutputPlan()
        model.cancelStart()
        XCTAssertEqual(model.state.status, .idle, "取消不启动")
        XCTAssertNil(model.state.tasks.first?.outputURL, "取消不写入输出计划")
        // 现有输出文件字节不变（取消路径零文件触碰）。
        let after = try Data(contentsOf: existingSRT)
        XCTAssertEqual(after, Data("existing srt bytes".utf8))
    }

    func testStartWithoutConflicts() throws {
        let a = try makeVideo("a.mp4")
        let model = makeModel()
        model.importInputs([a])
        model.prepareOutputPlan()
        XCTAssertTrue(model.outputConflicts.isEmpty)
        model.confirmOutputConflictsAndStart()
        XCTAssertEqual(model.state.status, .running)
        XCTAssertEqual(model.state.tasks.first?.outputURL?.lastPathComponent, "a.srt")
    }

    // MARK: - 显式改配置（08309 AC2：waiting 可改，活动/终态拒绝）

    func testReplaceTaskConfigurationWaitingAllowed() throws {
        let a = try makeVideo("a.mp4")
        let model = makeModel()
        model.importInputs([a])
        let taskID = model.state.tasks[0].id
        let result = model.replaceTaskConfiguration(taskID, ExtractionConfiguration(engine: .paddle, quality: .fine))
        XCTAssertTrue(result)
        XCTAssertEqual(model.state.tasks[0].configuration.engine, .paddle)
        XCTAssertEqual(model.state.tasks[0].configuration.quality, .fine)
    }

    func testReplaceTaskConfigurationRunningRejected() throws {
        let a = try makeVideo("a.mp4")
        let model = makeModel()
        model.importInputs([a])
        model.prepareOutputPlan()
        model.confirmOutputConflictsAndStart()
        // 启动后任务进入活动态——配置锁定。
        let taskID = model.state.tasks[0].id
        let result = model.replaceTaskConfiguration(taskID, ExtractionConfiguration(engine: .paddle, quality: .fine))
        XCTAssertFalse(result)
        XCTAssertEqual(model.state.tasks[0].configuration.engine, .vision)
    }

    func testReplaceTaskConfigurationNormalizesMockAway() throws {
        // P1 回归：普通 UI 不接受 Mock（app-wide"Mock 仅开发者模式"合同）。
        let a = try makeVideo("a.mp4")
        let model = makeModel()
        model.importInputs([a])
        let taskID = model.state.tasks[0].id
        let result = model.replaceTaskConfiguration(taskID, ExtractionConfiguration(engine: .mock, quality: .fast))
        XCTAssertTrue(result)
        XCTAssertEqual(model.state.tasks[0].configuration.engine, .vision, "Mock 归一化回落 Vision")
    }
}

// MARK: - 内存 Repository（测试辅助）

final class InMemoryBatchQueueRepository: BatchQueuePersisting, @unchecked Sendable {
    private var state = BatchQueueState.empty
    func load() throws -> BatchQueueState { state }
    func save(_ state: BatchQueueState) throws { self.state = state }
}

/// 立即完成的 Fake Runner（BatchQueueModel 构造用）。
final class FakeBatchRunner: BatchTaskRunning, @unchecked Sendable {
    func run(
        _ task: BatchTask,
        onUpdate: @escaping @Sendable (BatchTaskStatus, Double?) -> Void
    ) async -> BatchRunOutcome {
        onUpdate(.extracting, 0.5)
        return .completed(entryCount: 1, outputURL: task.outputURL ?? task.sourceURL, runtimeIdentity: "test")
    }
}
