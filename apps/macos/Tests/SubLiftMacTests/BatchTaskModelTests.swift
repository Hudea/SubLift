import XCTest
@testable import SubLiftMac

// MARK: - 状态转换纯状态机（08102）

final class BatchTaskStatusTests: XCTestCase {

    func testForwardChainIsLegal() {
        XCTAssertTrue(BatchTaskStatus.canTransition(from: .waiting, to: .preparing))
        XCTAssertTrue(BatchTaskStatus.canTransition(from: .preparing, to: .extracting))
        XCTAssertTrue(BatchTaskStatus.canTransition(from: .extracting, to: .exporting))
        XCTAssertTrue(BatchTaskStatus.canTransition(from: .exporting, to: .completed))
    }

    func testIllegalBackwardTransitionsRejected() {
        XCTAssertFalse(BatchTaskStatus.canTransition(from: .preparing, to: .waiting))
        XCTAssertFalse(BatchTaskStatus.canTransition(from: .extracting, to: .preparing))
        XCTAssertFalse(BatchTaskStatus.canTransition(from: .exporting, to: .extracting))
        XCTAssertFalse(BatchTaskStatus.canTransition(from: .completed, to: .exporting))
    }

    func testSkippingStagesRejected() {
        XCTAssertFalse(BatchTaskStatus.canTransition(from: .waiting, to: .extracting))
        XCTAssertFalse(BatchTaskStatus.canTransition(from: .waiting, to: .completed))
        XCTAssertFalse(BatchTaskStatus.canTransition(from: .preparing, to: .exporting))
        XCTAssertFalse(BatchTaskStatus.canTransition(from: .extracting, to: .completed))
    }

    func testFailureAndCancellationFromActiveStages() {
        for active in [BatchTaskStatus.preparing, .extracting, .exporting] {
            XCTAssertTrue(BatchTaskStatus.canTransition(from: active, to: .failed))
            XCTAssertTrue(BatchTaskStatus.canTransition(from: active, to: .cancelled))
            XCTAssertTrue(BatchTaskStatus.canTransition(from: active, to: .interrupted))
        }
    }

    func testWaitingCanCancelOrSkip() {
        XCTAssertTrue(BatchTaskStatus.canTransition(from: .waiting, to: .cancelled))
        XCTAssertTrue(BatchTaskStatus.canTransition(from: .waiting, to: .skipped))
        XCTAssertTrue(BatchTaskStatus.canTransition(from: .waiting, to: .interrupted))
    }

    func testTerminalStatesAreFinal() {
        for terminal in [BatchTaskStatus.completed, .failed, .cancelled, .interrupted, .skipped] {
            for target in BatchTaskStatus.allCases {
                XCTAssertFalse(BatchTaskStatus.canTransition(from: terminal, to: target), "终态 \(terminal) 不得转换到 \(target)")
            }
        }
    }
}

// MARK: - BatchTask 值类型（08102）

final class BatchTaskTests: XCTestCase {

    private func makeTask(
        engine: OcrEngineName = .vision,
        quality: SamplingQuality = .fast,
        developerMode: Bool = false,
        importRootURL: URL? = nil
    ) -> BatchTask {
        BatchTask.make(
            sourceURL: URL(fileURLWithPath: "/tmp/input.mp4"),
            engine: engine,
            quality: quality,
            developerMode: developerMode,
            importRootURL: importRootURL
        )
    }

    func testTaskStartsWithWaitingAndStableIdentity() {
        let task = makeTask()
        XCTAssertEqual(task.status, .waiting)
        XCTAssertNil(task.progress)
        XCTAssertNil(task.outputURL)
        XCTAssertNil(task.result)
        XCTAssertNil(task.failureMessage)
        XCTAssertNil(task.runToken)
        XCTAssertNil(task.importRootURL)
        XCTAssertEqual(task.id, task.id)
    }

    func testImportRootURLPreservedAtCreation() {
        let root = URL(fileURLWithPath: "/tmp/Zootopia")
        let task = makeTask(importRootURL: root)
        XCTAssertEqual(task.importRootURL, root)
    }

    func testTransitionUpdatesStatus() {
        var task = makeTask()
        XCTAssertTrue(task.transition(to: .preparing))
        XCTAssertEqual(task.status, .preparing)
        XCTAssertTrue(task.transition(to: .extracting))
        XCTAssertEqual(task.status, .extracting)
    }

    func testIllegalTransitionDoesNotMutate() {
        var task = makeTask()
        XCTAssertFalse(task.transition(to: .completed))
        XCTAssertEqual(task.status, .waiting, "非法转换不得改变状态")
    }

    func testWaitingCanReplaceConfigurationExplicitly() {
        var task = makeTask(engine: .vision, quality: .fast)
        let newConfig = ExtractionConfiguration(engine: .paddle, quality: .fine)
        XCTAssertTrue(task.replaceConfiguration(newConfig))
        XCTAssertEqual(task.configuration, newConfig)
    }

    func testConfigurationLockedAfterPreparing() {
        // 从 waiting 走合法链前进到各状态后，配置必须锁定。
        for status in [BatchTaskStatus.preparing, .extracting, .exporting, .completed, .failed, .cancelled] {
            var task = makeTask()
            advance(&task, to: status)
            XCTAssertEqual(task.status, status, "应前进到 \(status)")
            XCTAssertFalse(task.replaceConfiguration(.init(engine: .paddle, quality: .fine)), "\(status) 应锁定配置")
        }
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
            _ = task.transition(to: .preparing)
            _ = task.transition(to: .cancelled)
        default:
            break
        }
    }

    func testProgressClamped() {
        var task = makeTask()
        task.setProgress(1.5)
        XCTAssertEqual(task.progress, 1.0)
        task.setProgress(-0.2)
        XCTAssertEqual(task.progress, 0.0)
        task.setProgress(0.42)
        XCTAssertEqual(task.progress ?? -1, 0.42, accuracy: 0.001)
    }

    func testHiddenMockNormalizedAtCreation() {
        // 非开发模式：Mock 选择被归一化为 Vision（提取入口不能运行隐藏 Mock）。
        let task = makeTask(engine: .mock, quality: .fast, developerMode: false)
        XCTAssertEqual(task.configuration.engine, .vision)
        // 开发者模式保留 Mock。
        let devTask = makeTask(engine: .mock, quality: .fine, developerMode: true)
        XCTAssertEqual(devTask.configuration.engine, .mock)
    }

    func testStableIdentityAcrossCreationAndTransition() {
        let a = makeTask()
        let b = makeTask()
        XCTAssertNotEqual(a.id, b.id, "两次创建应产生不同 UUID")
        var task = a
        _ = task.transition(to: .preparing)
        XCTAssertEqual(task.id, a.id, "状态转换不得改变 id")
    }

    func testRecordFailureGuardedByActiveStatus() {
        var task = makeTask()
        task.recordFailure("等待中不可记录失败")
        XCTAssertNil(task.failureMessage)
        _ = task.transition(to: .preparing)
        task.recordFailure("启动失败")
        XCTAssertEqual(task.failureMessage, "启动失败")
        _ = task.transition(to: .failed)
        task.recordFailure("终态不可改写")
        XCTAssertEqual(task.failureMessage, "启动失败", "终态后不得改写失败摘要")
    }

    func testRecordResultGuardedByCompleted() {
        var task = makeTask()
        _ = task.transition(to: .preparing)
        task.recordResult(BatchTaskResult(entryCount: 1, outputURL: URL(fileURLWithPath: "/tmp/x.srt"), runtimeIdentity: nil))
        XCTAssertNil(task.result, "非 completed 状态不得写入结果")
        _ = task.transition(to: .failed)
        task.recordResult(BatchTaskResult(entryCount: 1, outputURL: URL(fileURLWithPath: "/tmp/x.srt"), runtimeIdentity: nil))
        XCTAssertNil(task.result)
    }

    func testQueueStateDerivedCollections() {
        var queue = BatchQueueState.empty
        var waiting = makeTask()
        waiting.outputURL = nil
        var active = makeTask()
        _ = active.transition(to: .preparing)
        var done = makeTask()
        _ = done.transition(to: .preparing)
        _ = done.transition(to: .failed)
        queue.tasks = [waiting, active, done]
        queue.runningTaskID = active.id
        XCTAssertEqual(queue.pendingTasks.count, 1)
        XCTAssertEqual(queue.pendingTasks.first?.id, waiting.id)
        XCTAssertEqual(queue.activeTasks.count, 1)
        XCTAssertEqual(queue.activeTasks.first?.id, active.id)
    }

    func testRequeueClearsFailureProgressAndResult() {
        // failed 任务含失败元数据；requeue 后应回到 waiting 并清除旧失败痕迹，
        // 但保留 outputURL（输出规划是用户意图，不应随重试丢失）。
        var task = makeTask()
        task.outputURL = URL(fileURLWithPath: "/tmp/output.srt")
        _ = task.transition(to: .preparing)
        task.setProgress(0.5)
        task.recordFailure("worker crashed")
        _ = task.transition(to: .failed)
        XCTAssertNotNil(task.failureMessage)
        XCTAssertNotNil(task.progress)

        task.requeue()

        XCTAssertEqual(task.status, .waiting)
        XCTAssertNil(task.failureMessage, "重试后旧错误摘要应清除")
        XCTAssertNil(task.progress, "重试后旧进度应清除")
        XCTAssertNil(task.result, "重试后旧结果应清除")
        XCTAssertNil(task.runToken, "重试后旧 run token 应清除")
        XCTAssertEqual(task.outputURL?.lastPathComponent, "output.srt", "重试应保留输出规划")
    }

    func testRequeueRejectedForCompletedAndWaiting() {
        var completed = makeTask()
        _ = completed.transition(to: .preparing)
        _ = completed.transition(to: .extracting)
        _ = completed.transition(to: .exporting)
        _ = completed.transition(to: .completed)
        completed.recordResult(BatchTaskResult(entryCount: 1, outputURL: URL(fileURLWithPath: "/tmp/output.srt"), runtimeIdentity: nil))
        completed.requeue()
        XCTAssertEqual(completed.status, .completed, "completed 不可 requeue")
        XCTAssertNotNil(completed.result, "completed 的 result 不应被 requeue 清除")

        var waiting = makeTask()
        waiting.requeue()
        XCTAssertEqual(waiting.status, .waiting, "waiting 不可 requeue")
    }
}

// MARK: - Codable 往返（08102）

final class BatchTaskCodableTests: XCTestCase {

    private let encoder = JSONEncoder()
    private let decoder = JSONDecoder()

    func testTaskCodableRoundTripPreservesFields() throws {
        let root = URL(fileURLWithPath: "/tmp/Zootopia")
        var task = BatchTask.make(
            sourceURL: URL(fileURLWithPath: "/tmp/input.mp4"),
            engine: .vision,
            quality: .balanced,
            developerMode: false,
            importRootURL: root
        )
        task.runToken = UUID()  // transient，不应持久化
        _ = task.transition(to: .preparing)
        task.setProgress(0.5)
        task.outputURL = URL(fileURLWithPath: "/tmp/input.srt")
        _ = task.transition(to: .exporting)
        _ = task.transition(to: .completed)
        task.recordResult(BatchTaskResult(entryCount: 12, outputURL: task.outputURL!, runtimeIdentity: "vision-cpp"))

        let data = try encoder.encode(task)
        let decoded = try decoder.decode(BatchTask.self, from: data)

        XCTAssertEqual(decoded.id, task.id)
        XCTAssertEqual(decoded.sourceURL, task.sourceURL)
        XCTAssertEqual(decoded.importRootURL, root)
        XCTAssertEqual(decoded.configuration, task.configuration)
        XCTAssertEqual(decoded.outputURL, task.outputURL)
        XCTAssertEqual(decoded.status, task.status)
        XCTAssertEqual(decoded.progress, task.progress)
        XCTAssertEqual(decoded.result, task.result)
        XCTAssertEqual(decoded.createdAt, task.createdAt)
        XCTAssertNil(decoded.runToken, "run token 不得进入持久化值")
        XCTAssertEqual(decoded, task, "相等性排除 runToken，持久化往返应相等")
    }

    func testTaskCodableMissingImportRootURLDecodesToNil() throws {
        // 旧清单（08102–08410）没有 importRootURL 键，应能加载且值为 nil。
        let oldJSON = """
        {
            "id": "550E8400-E29B-41D4-A716-446655440000",
            "sourceURL": "file:///tmp/input.mp4",
            "configuration": {"engine": "vision", "quality": "fast"},
            "status": "waiting",
            "createdAt": 600000000
        }
        """.data(using: .utf8)!
        let decoded = try decoder.decode(BatchTask.self, from: oldJSON)
        XCTAssertNil(decoded.importRootURL)
        XCTAssertEqual(decoded.sourceURL, URL(fileURLWithPath: "/tmp/input.mp4"))
        XCTAssertEqual(decoded.status, .waiting)
    }

    func testQueueStateCodableRoundTrip() throws {
        var queue = BatchQueueState(status: .running, tasks: [], runningTaskID: nil)
        var task = BatchTask.make(
            sourceURL: URL(fileURLWithPath: "/tmp/a.mp4"),
            engine: .vision,
            quality: .fast,
            developerMode: false
        )
        task.runToken = UUID()
        queue.tasks = [task]
        queue.runningTaskID = task.id

        let data = try encoder.encode(queue)
        let decoded = try decoder.decode(BatchQueueState.self, from: data)

        XCTAssertEqual(decoded.status, .running)
        XCTAssertEqual(decoded.tasks.count, 1)
        XCTAssertEqual(decoded.tasks[0].id, task.id)
        XCTAssertEqual(decoded.runningTaskID, task.id)
        XCTAssertNil(decoded.tasks[0].runToken)
    }

    func testConfigurationCodableRoundTrip() throws {
        let config = ExtractionConfiguration(engine: .paddle, quality: .fine)
        let data = try encoder.encode(config)
        let decoded = try decoder.decode(ExtractionConfiguration.self, from: data)
        XCTAssertEqual(decoded, config)
    }
}
