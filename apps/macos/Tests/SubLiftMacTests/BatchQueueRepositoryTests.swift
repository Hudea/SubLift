import XCTest
@testable import SubLiftMac

// MARK: - 队列持久化与恢复（08207）

final class BatchQueueRepositoryTests: XCTestCase {

    private var tempDir: URL!
    private var fileURL: URL!
    private var repository: BatchQueueRepository!
    private let fileManager = FileManager.default
    private let encoder: JSONEncoder = {
        let e = JSONEncoder()
        e.dateEncodingStrategy = .deferredToDate
        return e
    }()
    private let decoder: JSONDecoder = {
        let d = JSONDecoder()
        d.dateDecodingStrategy = .deferredToDate
        return d
    }()

    override func setUpWithError() throws {
        tempDir = fileManager.temporaryDirectory
            .appendingPathComponent("sublift-repo-\(UUID().uuidString)", isDirectory: true)
        try fileManager.createDirectory(at: tempDir, withIntermediateDirectories: true)
        fileURL = tempDir.appendingPathComponent("batch-queue-v1.json")
        repository = BatchQueueRepository(fileURL: fileURL)
    }

    override func tearDownWithError() throws {
        try? fileManager.setAttributes([.posixPermissions: 0o755], ofItemAtPath: tempDir.path)
        try fileManager.removeItem(at: tempDir)
    }

    private func makeTask(_ name: String, status: BatchTaskStatus = .waiting) -> BatchTask {
        var task = BatchTask.make(
            sourceURL: tempDir.appendingPathComponent(name),
            engine: .vision,
            quality: .fast,
            developerMode: false
        )
        if status != .waiting {
            advance(&task, to: status)
        }
        return task
    }

    /// 沿合法链前进（waiting→preparing→extracting→exporting→completed；活动态可 failed/cancelled）。
    private func advance(_ task: inout BatchTask, to target: BatchTaskStatus) {
        switch target {
        case .preparing:
            _ = task.transition(to: .preparing)
        case .extracting:
            _ = task.transition(to: .preparing)
            _ = task.transition(to: .extracting)
        case .exporting:
            _ = task.transition(to: .preparing)
            _ = task.transition(to: .extracting)
            _ = task.transition(to: .exporting)
        case .completed:
            _ = task.transition(to: .preparing)
            _ = task.transition(to: .extracting)
            _ = task.transition(to: .exporting)
            _ = task.transition(to: .completed)
        case .failed:
            _ = task.transition(to: .preparing)
            _ = task.transition(to: .failed)
        case .cancelled:
            _ = task.transition(to: .cancelled)
        case .interrupted:
            _ = task.transition(to: .preparing)
            _ = task.transition(to: .interrupted)
        case .skipped:
            _ = task.transition(to: .skipped)
        case .waiting:
            break
        }
    }

    // MARK: - round-trip

    func testRoundTripPreservesQueueAndTasks() throws {
        var queue = BatchQueueState.empty
        var t1 = makeTask("1.mp4")
        t1.outputURL = tempDir.appendingPathComponent("1.srt")
        var t2 = makeTask("2.mp4", status: .failed)
        t2.recordFailure("worker error")  // failed 是终态，recordFailure 守卫拒绝——用 preparing 记录
        var t3 = makeTask("3.mp4")
        _ = t3.transition(to: .preparing)
        t3.recordFailure("启动失败")
        queue.tasks = [t1, t2, t3]
        queue.status = .running
        queue.runningTaskID = t3.id

        try repository.save(queue)
        let loaded = try repository.load()

        XCTAssertEqual(loaded.tasks.count, 3)
        XCTAssertEqual(loaded.tasks[0], t1, "waiting 任务往返相等")
        XCTAssertEqual(loaded.tasks[1].status, .failed)
        XCTAssertEqual(loaded.tasks[2].status, .interrupted, "活动任务恢复为 interrupted")
        XCTAssertEqual(loaded.tasks[2].failureMessage, "启动失败")
        XCTAssertEqual(loaded.status, .paused, "运行中队列恢复为 paused")
        XCTAssertNil(loaded.runningTaskID)
    }

    func testLoadWhenFileMissingReturnsEmpty() throws {
        let loaded = try repository.load()
        XCTAssertEqual(loaded, .empty)
    }

    // MARK: - 原子故障

    func testSaveFailureKeepsOriginalFileIntact() throws {
        var queue = BatchQueueState.empty
        queue.tasks = [makeTask("1.mp4")]
        try repository.save(queue)

        let original = try Data(contentsOf: fileURL)
        // 故障注入：目录只读（tmp 写入阶段 EACCES）。
        try fileManager.setAttributes([.posixPermissions: 0o500], ofItemAtPath: tempDir.path)
        defer { try? fileManager.setAttributes([.posixPermissions: 0o755], ofItemAtPath: tempDir.path) }
        XCTAssertThrowsError(try repository.save(queue))

        let after = try Data(contentsOf: fileURL)
        XCTAssertEqual(after, original, "保存失败时原文件字节不变")
        let leftovers = try fileManager.contentsOfDirectory(atPath: tempDir.path)
            .filter { $0.contains(".tmp-") }
        XCTAssertTrue(leftovers.isEmpty, "临时文件必须被清理：\(leftovers)")
    }

    func testSaveSuccessLeavesNoTempFiles() throws {
        // 成功路径：原子替换后无 .tmp 残留（defer 清理在成功路径的执行锁定）。
        var queue = BatchQueueState.empty
        queue.tasks = [makeTask("1.mp4"), makeTask("2.mp4")]
        try repository.save(queue)
        try repository.save(queue)  // 覆盖路径（replaceItemAt）
        let leftovers = try fileManager.contentsOfDirectory(atPath: tempDir.path)
            .filter { $0.contains(".tmp-") }
        XCTAssertTrue(leftovers.isEmpty, "成功保存不得残留临时文件：\(leftovers)")
    }

    // MARK: - 损坏与未知版本

    func testCorruptedJSONFailsClosedAndPreservesFile() throws {
        var queue = BatchQueueState.empty
        queue.tasks = [makeTask("1.mp4")]
        try repository.save(queue)

        // 覆盖为损坏 JSON。
        let corrupted = Data("{ not valid json".utf8)
        try corrupted.write(to: fileURL)
        XCTAssertThrowsError(try repository.load()) { error in
            guard case BatchQueueRepositoryError.corrupted = error else {
                return XCTFail("损坏 JSON 应报 corrupted，实际 \(error)")
            }
        }
        XCTAssertEqual(try Data(contentsOf: fileURL), corrupted, "load 失败不得修改原文件")
    }

    func testUnknownSchemaVersionFailsClosed() throws {
        var queue = BatchQueueState.empty
        queue.tasks = [makeTask("1.mp4")]
        try repository.save(queue)
        let original = try Data(contentsOf: fileURL)

        // 改写 schemaVersion 为 99（合法 JSON）。
        let data = try Data(contentsOf: fileURL)
        var snapshot = try decoder.decode(BatchQueueSnapshot.self, from: data)
        snapshot = BatchQueueSnapshot(
            schemaVersion: 99,
            savedAt: snapshot.savedAt,
            tasks: snapshot.tasks,
            runningTaskID: snapshot.runningTaskID,
            status: snapshot.status
        )
        let writtenData = try encoder.encode(snapshot)
        try writtenData.write(to: fileURL)

        XCTAssertThrowsError(try repository.load()) { error in
            guard case BatchQueueRepositoryError.unknownSchemaVersion(let version) = error else {
                return XCTFail("未知版本应报 unknownSchemaVersion，实际 \(error)")
            }
            XCTAssertEqual(version, 99)
        }
        XCTAssertEqual(try Data(contentsOf: fileURL), writtenData, "未知版本文件不得被覆盖")
    }

    // MARK: - active 恢复

    func testActiveTasksRestoredToInterrupted() throws {
        var queue = BatchQueueState.empty
        var preparing = makeTask("p.mp4")
        _ = preparing.transition(to: .preparing)
        var extracting = makeTask("e.mp4")
        _ = extracting.transition(to: .preparing)
        _ = extracting.transition(to: .extracting)
        var exporting = makeTask("x.mp4")
        _ = exporting.transition(to: .preparing)
        _ = exporting.transition(to: .extracting)
        _ = exporting.transition(to: .exporting)
        var done = makeTask("d.mp4", status: .failed)
        queue.tasks = [preparing, extracting, exporting, done]
        queue.status = .running
        queue.runningTaskID = extracting.id

        try repository.save(queue)
        let loaded = try repository.load()
        XCTAssertEqual(loaded.tasks[0].status, .interrupted)
        XCTAssertEqual(loaded.tasks[1].status, .interrupted)
        XCTAssertEqual(loaded.tasks[2].status, .interrupted)
        XCTAssertEqual(loaded.tasks[3].status, .failed, "终态保持")
        XCTAssertEqual(loaded.status, .paused, "running → paused")
    }

    func testIdleSnapshotRestoresIdle() throws {
        // 恢复语义：仅 running → paused；idle/paused 快照保持原状态（计划定案）。
        var queue = BatchQueueState.empty
        queue.tasks = [makeTask("1.mp4"), makeTask("2.mp4", status: .failed)]
        queue.status = .idle
        try repository.save(queue)
        let loaded = try repository.load()
        XCTAssertEqual(loaded.status, .idle)

        queue.status = .paused
        try repository.save(queue)
        XCTAssertEqual(try repository.load().status, .paused)
    }

    func testRunTokenNotPersistedAndProgressResultRoundTrip() throws {
        var queue = BatchQueueState.empty
        var task = makeTask("1.mp4")
        task.runToken = UUID()  // 瞬态
        task.setProgress(0.5)
        _ = task.transition(to: .preparing)
        _ = task.transition(to: .extracting)
        task.setProgress(0.7)
        queue.tasks = [task]

        try repository.save(queue)
        let loaded = try repository.load()
        XCTAssertNil(loaded.tasks[0].runToken, "run token 不得持久化")
        XCTAssertEqual(loaded.tasks[0].status, .interrupted, "活动任务恢复为 interrupted")
        XCTAssertEqual(loaded.tasks[0].progress ?? -1, 0.7, accuracy: 0.001, "progress 字段保真")
    }

    func testErrorDescriptionsArePresentable() throws {
        XCTAssertTrue(BatchQueueRepositoryError.corrupted("x").errorDescription?.contains("损坏") == true)
        XCTAssertTrue(BatchQueueRepositoryError.unknownSchemaVersion(9).errorDescription?.contains("9") == true)
        XCTAssertTrue(BatchQueueRepositoryError.unknownSchemaVersion(9).errorDescription?.contains("1") == true)
    }

    // MARK: - 08511 outputDestination 持久化

    func testPublicRootDestinationRoundTripViaNewRepository() throws {
        let publicRoot = tempDir.appendingPathComponent("PublicOut", isDirectory: true)
        try fileManager.createDirectory(at: publicRoot, withIntermediateDirectories: true)
        var queue = BatchQueueState.empty
        queue.tasks = [makeTask("1.mp4")]
        queue.outputDestination = .publicRoot(publicRoot)

        try repository.save(queue)
        // 用新 repository 实例加载（避免内存缓存）。
        let newRepository = BatchQueueRepository(fileURL: fileURL)
        let loaded = try newRepository.load()
        XCTAssertEqual(loaded.outputDestination, .publicRoot(publicRoot))
    }

    func testOldSnapshotWithoutDestinationLoadsAsSidecar() throws {
        // 旧清单没有 outputDestination 键，应加载为 sidecar。
        let oldJSON = """
        {
            "schemaVersion": 1,
            "savedAt": 600000000,
            "tasks": [],
            "runningTaskID": null,
            "status": "idle"
        }
        """.data(using: .utf8)!
        try oldJSON.write(to: fileURL)
        let loaded = try repository.load()
        XCTAssertEqual(loaded.outputDestination, .sidecar)
    }

    func testQueueStateDecodeMissingDestinationDefaultsToSidecar() throws {
        let json = """
        {
            "status": "idle",
            "tasks": [],
            "runningTaskID": null
        }
        """.data(using: .utf8)!
        let decoded = try decoder.decode(BatchQueueState.self, from: json)
        XCTAssertEqual(decoded.outputDestination, .sidecar)
    }

    // MARK: - 100 task 体积与恢复时间

    func testHundredTaskManifestSizeAndRecoveryTime() throws {
        var queue = BatchQueueState.empty
        for i in 0..<100 {
            var task = makeTask("f\(i).mp4")
            task.outputURL = tempDir.appendingPathComponent("f\(i).srt")
            queue.tasks.append(task)
        }
        queue.status = .running

        try repository.save(queue)
        let fileSize = (try fileManager.attributesOfItem(atPath: fileURL.path)[.size] as? NSNumber)?.intValue ?? -1
        print("[08207] 100-task manifest bytes: \(fileSize)")

        let start = Date()
        let loaded = try repository.load()
        let elapsed = Date().timeIntervalSince(start)
        XCTAssertEqual(loaded.tasks.count, 100)
        XCTAssertLessThan(elapsed, 2.0, "100 任务恢复应 <2s（实际 \(elapsed)s）")
    }

    // MARK: - reset 显式新建

    func testResetDeletesManifest() throws {
        var queue = BatchQueueState.empty
        queue.tasks = [makeTask("1.mp4")]
        try repository.save(queue)
        XCTAssertTrue(fileManager.fileExists(atPath: fileURL.path))

        try repository.reset()
        XCTAssertFalse(fileManager.fileExists(atPath: fileURL.path))
        XCTAssertEqual(try repository.load(), .empty)
    }
}
