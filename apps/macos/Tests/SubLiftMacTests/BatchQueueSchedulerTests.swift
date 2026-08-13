import XCTest
@testable import SubLiftMac

// MARK: - Fake Runner（记录并发数，证明单并发与 teardown 顺序）

/// 行为驱动的 Fake Runner：按任务 ID 提供结果；记录最大同时活动数。
final class FakeRunner: BatchTaskRunning, @unchecked Sendable {
    private let lock = NSLock()
    private var behaviors: [UUID: @Sendable (@Sendable (BatchTaskStatus, Double?) -> Void) async -> BatchRunOutcome] = [:]
    private(set) var maxActive = 0
    private var active = 0
    private var runOrder: [UUID] = []

    func setBehavior(
        _ id: UUID,
        _ behavior: @escaping @Sendable (@Sendable (BatchTaskStatus, Double?) -> Void) async -> BatchRunOutcome
    ) {
        lock.lock()
        behaviors[id] = behavior
        lock.unlock()
    }

    var recordedMaxActive: Int {
        lock.lock()
        defer { lock.unlock() }
        return maxActive
    }

    var recordedRunOrder: [UUID] {
        lock.lock()
        defer { lock.unlock() }
        return runOrder
    }

    func run(
        _ task: BatchTask,
        onUpdate: @escaping @Sendable (BatchTaskStatus, Double?) -> Void
    ) async -> BatchRunOutcome {
        lock.lock()
        active += 1
        maxActive = max(maxActive, active)
        runOrder.append(task.id)
        lock.unlock()
        defer {
            lock.lock()
            active -= 1
            lock.unlock()
        }

        let behavior = lock.withLock { behaviors[task.id] }
        let outcome = await behavior?(onUpdate) ?? .completed(entryCount: 1, outputURL: URL(fileURLWithPath: "/tmp/out.srt"), runtimeIdentity: nil)
        return outcome
    }
}

// MARK: - 调度器测试（08205）

@MainActor
final class BatchQueueSchedulerTests: XCTestCase {

    private var runner: FakeRunner!
    private var repository: InMemoryQueueRepository!

    override func setUp() {
        runner = FakeRunner()
        repository = InMemoryQueueRepository()
    }

    private func makeTask(_ name: String = "a.mp4") -> BatchTask {
        BatchTask.make(sourceURL: URL(fileURLWithPath: "/tmp/\(name)"), engine: .vision, quality: .fast, developerMode: false)
    }

    private func makeScheduler(tasks: [BatchTask] = []) -> BatchQueueScheduler {
        var state = BatchQueueState.empty
        state.tasks = tasks
        return BatchQueueScheduler(runner: runner, repository: repository, initialState: state)
    }

    private func waitForIdle(_ scheduler: BatchQueueScheduler, timeout: TimeInterval = 5) async {
        await waitUntil(timeout: timeout) {
            scheduler.state.activeTasks.isEmpty && scheduler.state.status == .idle
        }
    }

    private func waitUntil(_ condition: @escaping () -> Bool, timeout: TimeInterval = 5) async {
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            if condition() { return }
            try? await Task.sleep(nanoseconds: 20_000_000)
        }
        XCTFail("条件未在 \(timeout)s 内满足")
    }

    // MARK: - 成功链与单并发

    func testSuccessChainRunsSequentiallyMaxActiveOne() async {
        let t1 = makeTask("1.mp4")
        let t2 = makeTask("2.mp4")
        let t3 = makeTask("3.mp4")
        runner.setBehavior(t1.id) { _ in .completed(entryCount: 3, outputURL: URL(fileURLWithPath: "/tmp/1.srt"), runtimeIdentity: nil) }
        runner.setBehavior(t2.id) { _ in .completed(entryCount: 4, outputURL: URL(fileURLWithPath: "/tmp/2.srt"), runtimeIdentity: nil) }
        runner.setBehavior(t3.id) { _ in .completed(entryCount: 5, outputURL: URL(fileURLWithPath: "/tmp/3.srt"), runtimeIdentity: nil) }

        let scheduler = makeScheduler(tasks: [t1, t2, t3])
        scheduler.start()
        await waitForIdle(scheduler)

        XCTAssertEqual(scheduler.state.tasks.map(\.status), [.completed, .completed, .completed])
        XCTAssertEqual(runner.recordedMaxActive, 1, "任意时刻最多一个活动任务")
        XCTAssertEqual(runner.recordedRunOrder, [t1.id, t2.id, t3.id], "严格顺序执行")
        XCTAssertEqual(scheduler.state.tasks[1].result?.entryCount, 4)
    }

    // MARK: - 失败继续

    func testFailureContinuesToNextTask() async {
        let t1 = makeTask("1.mp4")
        let t2 = makeTask("2.mp4")
        runner.setBehavior(t1.id) { _ in .failed("worker error") }
        runner.setBehavior(t2.id) { _ in .completed(entryCount: 2, outputURL: URL(fileURLWithPath: "/tmp/2.srt"), runtimeIdentity: nil) }

        let scheduler = makeScheduler(tasks: [t1, t2])
        scheduler.start()
        await waitForIdle(scheduler)

        XCTAssertEqual(scheduler.state.tasks[0].status, .failed)
        XCTAssertEqual(scheduler.state.tasks[0].failureMessage, "worker error")
        XCTAssertEqual(scheduler.state.tasks[1].status, .completed)
        XCTAssertEqual(runner.recordedMaxActive, 1)
    }

    // MARK: - 暂停/恢复

    func testPauseAfterCurrentThenResume() async {
        let t1 = makeTask("1.mp4")
        let t2 = makeTask("2.mp4")
        let t1Gate = AsyncGate()
        runner.setBehavior(t1.id) { _ in
            await t1Gate.waitUntilOpened()
            return .completed(entryCount: 1, outputURL: URL(fileURLWithPath: "/tmp/1.srt"), runtimeIdentity: nil)
        }
        runner.setBehavior(t2.id) { _ in .completed(entryCount: 2, outputURL: URL(fileURLWithPath: "/tmp/2.srt"), runtimeIdentity: nil) }

        let scheduler = makeScheduler(tasks: [t1, t2])
        scheduler.start()
        scheduler.pauseAfterCurrent()
        // 当前任务完成后暂停。
        await t1Gate.open()
        await waitUntil { scheduler.state.status == .paused }
        XCTAssertEqual(scheduler.state.tasks[0].status, .completed)
        XCTAssertEqual(scheduler.state.tasks[1].status, .waiting, "暂停后不启动下一项")

        // 恢复。
        scheduler.resume()
        await waitForIdle(scheduler)
        XCTAssertEqual(scheduler.state.tasks[1].status, .completed)
    }

    // MARK: - 停止

    func testStopCancelsCurrentAndDoesNotStartNext() async {
        let t1 = makeTask("1.mp4")
        let t2 = makeTask("2.mp4")
        let t1Gate = AsyncGate()
        runner.setBehavior(t1.id) { _ in
            await t1Gate.waitUntilOpened()
            if Task.isCancelled { return .cancelled }
            return .completed(entryCount: 1, outputURL: URL(fileURLWithPath: "/tmp/1.srt"), runtimeIdentity: nil)
        }
        runner.setBehavior(t2.id) { _ in .completed(entryCount: 2, outputURL: URL(fileURLWithPath: "/tmp/2.srt"), runtimeIdentity: nil) }

        let scheduler = makeScheduler(tasks: [t1, t2])
        scheduler.start()
        scheduler.stop()  // 当前任务取消 + 不再调度
        await t1Gate.open()
        await waitUntil { scheduler.state.status == .paused }
        XCTAssertEqual(scheduler.state.tasks[0].status, .cancelled, "stop 取消当前任务")
        XCTAssertEqual(scheduler.state.tasks[1].status, .waiting, "等待项保持顺序")
        XCTAssertEqual(runner.recordedRunOrder, [t1.id], "不启动下一项")
    }

    // MARK: - 取消

    func testCancelWaitingTask() {
        let t1 = makeTask("1.mp4")
        let t2 = makeTask("2.mp4")
        let scheduler = makeScheduler(tasks: [t1, t2])
        XCTAssertTrue(scheduler.cancel(t1.id))
        XCTAssertEqual(scheduler.state.tasks[0].status, .cancelled)
    }

    func testCancelRunningTaskThenContinue() async {
        let t1 = makeTask("1.mp4")
        let t2 = makeTask("2.mp4")
        let t1Gate = AsyncGate()
        runner.setBehavior(t1.id) { _ in
            await t1Gate.waitUntilOpened()
            if Task.isCancelled { return .cancelled }
            return .cancelled
        }
        runner.setBehavior(t2.id) { _ in .completed(entryCount: 2, outputURL: URL(fileURLWithPath: "/tmp/2.srt"), runtimeIdentity: nil) }

        let scheduler = makeScheduler(tasks: [t1, t2])
        scheduler.start()
        XCTAssertTrue(scheduler.cancel(t1.id), "运行中任务可取消")
        await t1Gate.open()
        await waitForIdle(scheduler)
        XCTAssertEqual(scheduler.state.tasks[0].status, .cancelled)
        XCTAssertEqual(scheduler.state.tasks[1].status, .completed, "取消后调度继续")
        XCTAssertEqual(runner.recordedMaxActive, 1)
    }

    // MARK: - onStateChange（08308 live 更新）

    func testOnStateChangeFiresOnCommandAndProgress() async {
        let t1 = makeTask("1.mp4")
        let t2 = makeTask("2.mp4")
        runner.setBehavior(t1.id) { onUpdate in
            onUpdate(.extracting, 0.5)
            onUpdate(.extracting, 0.9)
            return .completed(entryCount: 1, outputURL: URL(fileURLWithPath: "/tmp/1.srt"), runtimeIdentity: "cpp")
        }
        runner.setBehavior(t2.id) { _ in
            .completed(entryCount: 2, outputURL: URL(fileURLWithPath: "/tmp/2.srt"), runtimeIdentity: "cpp")
        }
        let scheduler = makeScheduler(tasks: [t1, t2])
        var received: [BatchQueueState] = []
        scheduler.onStateChange = { received.append($0) }

        scheduler.start()
        // 进度触发（0.5 到达 onUpdate → onStateChange 收集）。
        await waitUntil { received.contains { $0.tasks.first?.progress == 0.5 } }
        await waitForIdle(scheduler)

        // 命令（start）+ 进度（0.5）+ 终态（completed 后 idle）均触发回调。
        XCTAssertGreaterThan(received.count, 2, "命令/进度/终态都应有回调")
        XCTAssertTrue(received.contains { $0.tasks.first?.progress == 0.5 })
        XCTAssertEqual(received.last?.status, .idle)
        XCTAssertEqual(received.last?.tasks.first?.status, .completed)
    }

    func testOnStateChangeNotCalledWhenNoSubscription() async {
        // 无订阅者时调度不受影响（回调可选）。
        let t1 = makeTask("1.mp4")
        runner.setBehavior(t1.id) { _ in
            .completed(entryCount: 1, outputURL: URL(fileURLWithPath: "/tmp/1.srt"), runtimeIdentity: "cpp")
        }
        let scheduler = makeScheduler(tasks: [t1])
        scheduler.start()
        await waitForIdle(scheduler)
        XCTAssertEqual(scheduler.state.tasks.first?.status, .completed)
    }

    // MARK: - 迟到事件拒绝

    func testLateUpdateAfterCancellationIgnored() async {
        let t1 = makeTask("1.mp4")
        let t2 = makeTask("2.mp4")
        let t1Gate = AsyncGate()
        runner.setBehavior(t1.id) { onUpdate in
            // 模拟迟到回调：即使任务已取消，仍发出 extracting + progress。
            await t1Gate.waitUntilOpened()
            onUpdate(.extracting, 0.9)
            return .cancelled
        }
        runner.setBehavior(t2.id) { _ in .completed(entryCount: 2, outputURL: URL(fileURLWithPath: "/tmp/2.srt"), runtimeIdentity: nil) }

        let scheduler = makeScheduler(tasks: [t1, t2])
        scheduler.start()
        scheduler.cancel(t1.id)
        await t1Gate.open()
        await waitForIdle(scheduler)

        // 迟到更新被拒绝：t1 保持 cancelled，不回到 extracting。
        XCTAssertEqual(scheduler.state.tasks[0].status, .cancelled)
        XCTAssertNil(scheduler.state.tasks[0].progress)
    }

    // MARK: - onUpdate 语义（P1 回归：同阶段 progress 保留、终态拒绝）

    func testSameStageProgressUpdatesRetained() async {
        // 同阶段多次进度上报必须保留（progress 与状态转换解耦）。
        let t1 = makeTask("1.mp4")
        runner.setBehavior(t1.id) { onUpdate in
            onUpdate(.extracting, 0.1)
            onUpdate(.extracting, 0.5)
            onUpdate(.extracting, 0.9)
            return .completed(entryCount: 3, outputURL: URL(fileURLWithPath: "/tmp/1.srt"), runtimeIdentity: nil)
        }
        let scheduler = makeScheduler(tasks: [t1])
        scheduler.start()
        await waitForIdle(scheduler)
        XCTAssertEqual(scheduler.state.tasks[0].status, .completed)
        // 终态完成时进度保留（runner 报告的最后值；完成不重置）。
        XCTAssertEqual(scheduler.state.tasks[0].progress ?? -1, 0.9, accuracy: 0.001)
    }

    func testOnUpdateTerminalStatusRejected() async {
        // onUpdate 只接受活动态：runner 通过 onUpdate 报 .failed 被拒绝，
        // 终态只走 outcome 通道（failureMessage 不丢失）。
        let t1 = makeTask("1.mp4")
        runner.setBehavior(t1.id) { onUpdate in
            onUpdate(.failed, nil)  // 应被拒绝（非活动态）
            return .failed("worker error")
        }
        let scheduler = makeScheduler(tasks: [t1])
        scheduler.start()
        await waitForIdle(scheduler)
        XCTAssertEqual(scheduler.state.tasks[0].status, .failed)
        XCTAssertEqual(scheduler.state.tasks[0].failureMessage, "worker error", "终态必须经 outcome 通道记录失败摘要")
    }

    // MARK: - retry

    func testRetryFailedTaskRequeues() async {
        let t1 = makeTask("1.mp4")
        runner.setBehavior(t1.id) { _ in .failed("first try") }

        let scheduler = makeScheduler(tasks: [t1])
        scheduler.start()
        await waitForIdle(scheduler)
        XCTAssertEqual(scheduler.state.tasks[0].status, .failed)

        // retry：failed → waiting，新 run token 重新排队。
        runner.setBehavior(t1.id) { _ in .completed(entryCount: 7, outputURL: URL(fileURLWithPath: "/tmp/1.srt"), runtimeIdentity: nil) }
        XCTAssertTrue(scheduler.retry(t1.id))
        XCTAssertEqual(scheduler.state.tasks[0].status, .waiting)
        scheduler.start()
        await waitForIdle(scheduler)
        XCTAssertEqual(scheduler.state.tasks[0].status, .completed)
        XCTAssertEqual(scheduler.state.tasks[0].result?.entryCount, 7)
        XCTAssertEqual(runner.recordedRunOrder, [t1.id, t1.id], "retry 再次运行")
    }

    func testCompletedCannotRetry() {
        let t1 = makeTask("d.mp4")
        var task = t1
        _ = task.transition(to: .preparing)
        _ = task.transition(to: .extracting)
        _ = task.transition(to: .exporting)
        _ = task.transition(to: .completed)
        let scheduler = makeScheduler(tasks: [task])
        XCTAssertFalse(scheduler.retry(t1.id), "completed 不可重跑")
        XCTAssertTrue(scheduler.remove(t1.id), "已完成可移出队列")
    }

    func testRetryFailedTaskClearsFailureAndResult() async {
        let t1 = makeTask("1.mp4")
        runner.setBehavior(t1.id) { _ in .failed("first try") }

        let scheduler = makeScheduler(tasks: [t1])
        scheduler.start()
        await waitForIdle(scheduler)

        XCTAssertEqual(scheduler.state.tasks[0].status, .failed)
        XCTAssertEqual(scheduler.state.tasks[0].failureMessage, "first try")

        runner.setBehavior(t1.id) { _ in
            .completed(entryCount: 5, outputURL: URL(fileURLWithPath: "/tmp/1.srt"), runtimeIdentity: nil)
        }
        XCTAssertTrue(scheduler.retry(t1.id))
        XCTAssertEqual(scheduler.state.tasks[0].status, .waiting)
        XCTAssertNil(scheduler.state.tasks[0].failureMessage, "retry 应清除旧失败摘要")
        XCTAssertNil(scheduler.state.tasks[0].result, "retry 应清除旧结果")

        scheduler.start()
        await waitForIdle(scheduler)

        XCTAssertEqual(scheduler.state.tasks[0].status, .completed)
        XCTAssertEqual(scheduler.state.tasks[0].result?.entryCount, 5)
        XCTAssertEqual(runner.recordedRunOrder, [t1.id, t1.id])
    }

    // MARK: - 仅 waiting 的命令

    func testRemoveReorderReplaceOnlyForWaiting() {
        let t1 = makeTask("1.mp4")
        let t2 = makeTask("2.mp4")
        let scheduler = makeScheduler(tasks: [t1, t2])

        // waiting 可删除/重排/改配置。
        XCTAssertTrue(scheduler.replaceConfiguration(t1.id, ExtractionConfiguration(engine: .paddle, quality: .fine)))
        XCTAssertEqual(scheduler.state.tasks[0].configuration.quality, .fine)
        XCTAssertTrue(scheduler.reorder([t2.id, t1.id]))
        XCTAssertEqual(scheduler.state.tasks.map(\.id), [t2.id, t1.id])
        XCTAssertTrue(scheduler.remove(t2.id))
        XCTAssertEqual(scheduler.state.tasks.map(\.id), [t1.id])

        // 活动态不可删；失败终态可移出队列。
        var running = makeTask("r.mp4")
        _ = running.transition(to: .preparing)
        var failed = makeTask("d.mp4")
        _ = failed.transition(to: .preparing)
        failed.recordFailure("x")
        _ = failed.transition(to: .failed)
        let s2 = makeScheduler(tasks: [running, failed])
        XCTAssertFalse(s2.remove(running.id))
        XCTAssertTrue(s2.remove(failed.id))
        XCTAssertEqual(s2.state.tasks.map(\.id), [running.id])
        XCTAssertFalse(s2.reorder([failed.id, running.id]))
        XCTAssertFalse(s2.replaceConfiguration(running.id, .init(engine: .vision, quality: .fast)))
    }

    func testStartSingleRunsOnlySelectedTaskThenPauses() async {
        let first = makeTask("a.mp4")
        let second = makeTask("b.mp4")
        let selected = makeTask("c.mp4")
        let scheduler = makeScheduler(tasks: [first, second, selected])
        XCTAssertTrue(scheduler.startSingle(selected.id))
        await waitUntil {
            scheduler.state.tasks[2].status == .completed && scheduler.state.status == .paused
        }
        XCTAssertEqual(runner.recordedRunOrder, [selected.id], "只应启动选中项")
        XCTAssertEqual(scheduler.state.status, .paused)
        XCTAssertEqual(scheduler.state.tasks[0].status, .waiting)
        XCTAssertEqual(scheduler.state.tasks[1].status, .waiting)
        XCTAssertEqual(scheduler.state.tasks[2].status, .completed)
    }

    func testStartSingleRejectedWhenNotWaiting() {
        var failed = makeTask("f.mp4")
        _ = failed.transition(to: .preparing)
        failed.recordFailure("x")
        _ = failed.transition(to: .failed)
        let scheduler = makeScheduler(tasks: [failed])
        XCTAssertFalse(scheduler.startSingle(failed.id))
    }

    func testAvailabilitySingleSourceOfTruth() {
        // 唯一 availability 真源与命令一致。
        XCTAssertTrue(BatchTaskCommandAvailability.canRetry(.failed))
        XCTAssertTrue(BatchTaskCommandAvailability.canRetry(.cancelled))
        XCTAssertTrue(BatchTaskCommandAvailability.canRetry(.interrupted))
        XCTAssertFalse(BatchTaskCommandAvailability.canRetry(.completed))
        XCTAssertFalse(BatchTaskCommandAvailability.canRetry(.waiting))
        XCTAssertTrue(BatchTaskCommandAvailability.canRemove(.waiting))
        XCTAssertTrue(BatchTaskCommandAvailability.canRemove(.failed))
        XCTAssertTrue(BatchTaskCommandAvailability.canRemove(.completed))
        XCTAssertFalse(BatchTaskCommandAvailability.canRemove(.preparing))
        XCTAssertTrue(BatchTaskCommandAvailability.canStartSingle(.waiting))
        XCTAssertFalse(BatchTaskCommandAvailability.canStartSingle(.failed))
        XCTAssertTrue(BatchTaskCommandAvailability.canCancel(.preparing))
        XCTAssertTrue(BatchTaskCommandAvailability.canCancel(.waiting))
        XCTAssertTrue(BatchTaskCommandAvailability.isActive(.extracting))
        XCTAssertFalse(BatchTaskCommandAvailability.isActive(.waiting))
    }

    // MARK: - 08511 输出目的地与 outputURL 写入

    func testSetOutputURLAllowedForRetryableStates() {
        var failed = makeTask("f.mp4")
        _ = failed.transition(to: .preparing)
        failed.recordFailure("error")
        _ = failed.transition(to: .failed)
        var cancelled = makeTask("c.mp4")
        _ = cancelled.transition(to: .cancelled)
        var interrupted = makeTask("i.mp4")
        _ = interrupted.transition(to: .preparing)
        _ = interrupted.transition(to: .interrupted)

        let scheduler = makeScheduler(tasks: [failed, cancelled, interrupted])
        let newURL = URL(fileURLWithPath: "/tmp/new.srt")
        XCTAssertTrue(scheduler.setOutputURL(newURL, for: failed.id))
        XCTAssertTrue(scheduler.setOutputURL(newURL, for: cancelled.id))
        XCTAssertTrue(scheduler.setOutputURL(newURL, for: interrupted.id))
        XCTAssertEqual(scheduler.state.tasks[0].outputURL, newURL)
        XCTAssertEqual(scheduler.state.tasks[1].outputURL, newURL)
        XCTAssertEqual(scheduler.state.tasks[2].outputURL, newURL)
        XCTAssertEqual(scheduler.state.tasks[0].status, .failed, "只改 outputURL，不改状态")
    }

    func testSetOutputURLRejectedForCompleted() {
        var completed = makeTask("d.mp4")
        _ = completed.transition(to: .preparing)
        _ = completed.transition(to: .extracting)
        _ = completed.transition(to: .exporting)
        _ = completed.transition(to: .completed)
        let scheduler = makeScheduler(tasks: [completed])
        XCTAssertFalse(scheduler.setOutputURL(URL(fileURLWithPath: "/tmp/x.srt"), for: completed.id))
    }

    func testSetOutputURLsBatchPersistsOnce() {
        let t1 = makeTask("1.mp4")
        let t2 = makeTask("2.mp4")
        let scheduler = makeScheduler(tasks: [t1, t2])
        let url1 = URL(fileURLWithPath: "/tmp/1.srt")
        let url2 = URL(fileURLWithPath: "/tmp/2.srt")
        XCTAssertTrue(scheduler.setOutputURLs([t1.id: url1, t2.id: url2]))
        XCTAssertEqual(scheduler.state.tasks[0].outputURL, url1)
        XCTAssertEqual(scheduler.state.tasks[1].outputURL, url2)
    }

    func testSetOutputDestinationPersistsToRepository() {
        let scheduler = makeScheduler()
        let publicRoot = URL(fileURLWithPath: "/tmp/PublicOut")
        scheduler.setOutputDestination(.publicRoot(publicRoot))
        XCTAssertEqual(scheduler.state.outputDestination, .publicRoot(publicRoot))
        let loaded = (try? repository.load()) ?? .empty
        XCTAssertEqual(loaded.outputDestination, .publicRoot(publicRoot))
    }

    // MARK: - 大量等待任务

    func testManyWaitingTasksRunSequentially() async {
        var tasks: [BatchTask] = []
        for i in 0..<50 {
            let t = makeTask("\(i).mp4")
            runner.setBehavior(t.id) { _ in .completed(entryCount: 1, outputURL: URL(fileURLWithPath: "/tmp/\(i).srt"), runtimeIdentity: nil) }
            tasks.append(t)
        }
        let scheduler = makeScheduler(tasks: tasks)
        scheduler.start()
        await waitForIdle(scheduler, timeout: 10)
        XCTAssertEqual(scheduler.state.tasks.filter { $0.status == .completed }.count, 50)
        XCTAssertEqual(runner.recordedMaxActive, 1)
    }
}

// MARK: - 测试辅助

/// 内存 Repository（08205 测试用；08207 实现 JSON 持久化）。
final class InMemoryQueueRepository: BatchQueuePersisting, @unchecked Sendable {
    private var stored: BatchQueueState?
    func load() throws -> BatchQueueState { stored ?? .empty }
    func save(_ state: BatchQueueState) throws { stored = state }
}

/// 异步门控（测试同步）。
final class AsyncGate: @unchecked Sendable {
    private let semaphore = DispatchSemaphore(value: 0)
    func waitUntilOpened() async {
        await withCheckedContinuation { continuation in
            DispatchQueue.global().async {
                self.semaphore.wait()
                continuation.resume()
            }
        }
    }
    func open() {
        semaphore.signal()
    }
}

