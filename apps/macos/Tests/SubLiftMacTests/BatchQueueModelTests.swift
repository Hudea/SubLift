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

    func testImportInputsPreservesImportRootURL() throws {
        let folder = try makeSubdir("Zootopia")
        let folderVideo = try makeVideo("Zootopia/movie.mp4")
        let looseVideo = try makeVideo("bonus.mp4")
        let model = makeModel()
        model.importInputs([folder, looseVideo])

        let folderTask = model.state.tasks.first { $0.sourceURL == folderVideo.standardizedFileURL }
        let looseTask = model.state.tasks.first { $0.sourceURL == looseVideo.standardizedFileURL }
        XCTAssertEqual(folderTask?.importRootURL, folder.standardizedFileURL)
        XCTAssertNil(looseTask?.importRootURL)
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
        let previewedURL = model.state.tasks.first?.outputURL
        XCTAssertNotNil(previewedURL, "导入后应已有预览 outputURL")
        model.prepareOutputPlan()
        model.cancelStart()
        XCTAssertEqual(model.state.status, .idle, "取消不启动")
        XCTAssertEqual(model.state.tasks.first?.outputURL, previewedURL, "取消开始保留预览 outputURL")
        // 现有输出文件字节不变（取消路径零文件触碰）。
        let after = try Data(contentsOf: existingSRT)
        XCTAssertEqual(after, Data("existing srt bytes".utf8))
    }

    func testImportPlansPreviewOutputURL() throws {
        let a = try makeVideo("a.mp4")
        let model = makeModel()
        model.importInputs([a])

        XCTAssertEqual(model.state.tasks.count, 1)
        XCTAssertEqual(model.state.tasks[0].outputURL?.lastPathComponent, "a.srt")
        XCTAssertEqual(model.state.tasks[0].status, .waiting, "导入后仍是 waiting")
        XCTAssertNil(model.state.tasks[0].failureMessage)
        // 磁盘上无新 SRT（仅预览）。
        XCTAssertFalse(FileManager.default.fileExists(atPath: tempDir.appendingPathComponent("a.srt").path))
    }

    func testImportWithExistingSRTKeepsWaitingAndSetsOutputURL() throws {
        let a = try makeVideo("a.mp4")
        let existingSRT = tempDir.appendingPathComponent("a.srt")
        try Data("existing".utf8).write(to: existingSRT)
        let model = makeModel()
        model.importInputs([a])

        XCTAssertEqual(model.state.tasks[0].status, .waiting, "目标已存在也不 skipped")
        XCTAssertEqual(model.state.tasks[0].outputURL, existingSRT.standardizedFileURL)
        XCTAssertNil(model.state.tasks[0].failureMessage)
        XCTAssertFalse(model.planningErrors.keys.contains(model.state.tasks[0].id))
    }

    func testPublicRootPreviewMatchesPreparePlanForSameBasenameFolders() throws {
        let publicRoot = tempDir.appendingPathComponent("Out", isDirectory: true)
        try fileManager.createDirectory(at: publicRoot, withIntermediateDirectories: true)
        let folderA = tempDir.appendingPathComponent("A", isDirectory: true)
        let folderB = tempDir.appendingPathComponent("B", isDirectory: true)
        try fileManager.createDirectory(at: folderA, withIntermediateDirectories: true)
        try fileManager.createDirectory(at: folderB, withIntermediateDirectories: true)
        let videoA = try makeVideo("A/movie.mp4")
        let videoB = try makeVideo("B/movie.mp4")

        let model = makeModel()
        model.setOutputDestination(.publicRoot(publicRoot))
        model.importInputs([folderA, folderB])

        let taskA = model.state.tasks.first { $0.sourceURL == videoA.standardizedFileURL }!
        let taskB = model.state.tasks.first { $0.sourceURL == videoB.standardizedFileURL }!
        // 两文件夹分别作为 sourceRoot，basename 相同 → 预览目标相同 → 同批碰撞。
        let previewedTargets = [taskA.outputURL, taskB.outputURL].compactMap { $0 }
        XCTAssertEqual(Set(previewedTargets).count, 1, "两个 movie.mp4 的预览目标应相同")
        XCTAssertNotNil(model.planningErrors[taskA.id] ?? model.planningErrors[taskB.id], "后者应有 planningError")

        // 再跑 prepareOutputPlan：目标应与 preview 一致（sourceRootForPlanning 相同）。
        model.prepareOutputPlan()
        let preparedTargets = [taskA.outputURL, taskB.outputURL].compactMap { $0 }
        XCTAssertEqual(Set(preparedTargets).count, 1, "prepare 目标与 preview 一致")
        XCTAssertNotNil(model.planningErrors[taskA.id] ?? model.planningErrors[taskB.id])
        XCTAssertEqual(model.state.status, .idle, "有碰撞不启动")
    }

    func testBatchCollisionFirstComeFirstServed() throws {
        let publicRoot = tempDir.appendingPathComponent("Out", isDirectory: true)
        try fileManager.createDirectory(at: publicRoot, withIntermediateDirectories: true)
        // 两个独立文件同 basename、nil sourceRoot → 公共根下目标相同 → 同批碰撞。
        let dirA = tempDir.appendingPathComponent("A", isDirectory: true)
        let dirB = tempDir.appendingPathComponent("B", isDirectory: true)
        try fileManager.createDirectory(at: dirA, withIntermediateDirectories: true)
        try fileManager.createDirectory(at: dirB, withIntermediateDirectories: true)
        let a = try makeVideo("A/movie.mp4")
        let b = try makeVideo("B/movie.mp4")

        let model = makeModel()
        model.setOutputDestination(.publicRoot(publicRoot))
        model.importInputs([a, b])

        let taskA = model.state.tasks.first { $0.sourceURL == a.standardizedFileURL }!
        let taskB = model.state.tasks.first { $0.sourceURL == b.standardizedFileURL }!
        // 直接文件输入 sourceRoot == nil，公共根下都用 basename → 同名碰撞。
        XCTAssertEqual(taskA.outputURL, publicRoot.appendingPathComponent("movie.srt"))
        // 先到先得：taskA 有 outputURL，taskB 被跳过（outputURL 保持 nil）。
        XCTAssertNil(taskB.outputURL)
        // 后者 planningError，没有 failureMessage，保持 waiting。
        XCTAssertNotNil(model.planningErrors[taskB.id])
        XCTAssertNil(taskA.failureMessage)
        XCTAssertNil(taskB.failureMessage)
        XCTAssertEqual(taskA.status, .waiting)
        XCTAssertEqual(taskB.status, .waiting)
        // prepareOutputPlan 也不启动。
        model.prepareOutputPlan()
        XCTAssertEqual(model.state.status, .idle)
    }

    func testRetryableTaskThenChangePublicRootUpdatesOutputURLBeforeRetry() throws {
        let publicRoot = tempDir.appendingPathComponent("Out", isDirectory: true)
        try fileManager.createDirectory(at: publicRoot, withIntermediateDirectories: true)
        let a = try makeVideo("a.mp4")
        let model = makeModel()
        model.importInputs([a])
        let sidecarURL = model.state.tasks[0].outputURL
        XCTAssertEqual(sidecarURL?.deletingLastPathComponent(), tempDir.standardizedFileURL)

        // 模拟任务进入可重试状态（cancelled）。
        _ = model.cancel(model.state.tasks[0].id)
        XCTAssertEqual(model.state.tasks[0].status, .cancelled)

        // 切换公共根（不重试）——outputURL 应已更新到新根。
        model.setOutputDestination(.publicRoot(publicRoot))
        let publicURL = model.state.tasks[0].outputURL
        XCTAssertEqual(publicURL?.deletingLastPathComponent(), publicRoot.standardizedFileURL)
        XCTAssertEqual(model.state.tasks[0].status, .cancelled, "只改预览路径，不改 status")
    }

    func testPrepareOutputPlanWithUnwritableDirectoryDoesNotStart() throws {
        let lockedDir = tempDir.appendingPathComponent("locked", isDirectory: true)
        try fileManager.createDirectory(at: lockedDir, withIntermediateDirectories: true)
        let a = try makeVideo("locked/a.mp4")
        try fileManager.setAttributes([.posixPermissions: 0o500], ofItemAtPath: lockedDir.path)
        defer { try? fileManager.setAttributes([.posixPermissions: 0o755], ofItemAtPath: lockedDir.path) }

        let model = makeModel()
        model.importInputs([a])
        model.prepareOutputPlan()

        XCTAssertEqual(model.state.status, .idle, "不可写目录不应启动")
        XCTAssertNotNil(model.planningErrors[model.state.tasks[0].id])
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
