import Foundation

/// 08205：任务命令可用性唯一真源（UI 与 Scheduler 共用）。
enum BatchTaskCommandAvailability {
    static func isActive(_ status: BatchTaskStatus) -> Bool {
        status == .preparing || status == .extracting || status == .exporting
    }

    static func canRemove(_ status: BatchTaskStatus) -> Bool {
        // 活动态不可删（会拆掉正在跑的 Worker）；waiting 与全部终态可移出队列。
        !isActive(status)
    }
    static func canReorder(_ status: BatchTaskStatus) -> Bool { status == .waiting }
    static func canReplaceConfiguration(_ status: BatchTaskStatus) -> Bool { status == .waiting }
    static func canCancel(_ status: BatchTaskStatus) -> Bool { status == .waiting || isActive(status) }
    static func canRetry(_ status: BatchTaskStatus) -> Bool {
        status == .failed || status == .cancelled || status == .interrupted
    }
    static func canStartSingle(_ status: BatchTaskStatus) -> Bool { status == .waiting }
}

/// 08205：运行时暂停原因（不持久化）。与 `.running` 组合派生 draining 展示态。
/// 仅 `.afterCurrent` 可经 clearPauseRequest 撤销；`.singleRun`/`.stopped`
/// 是单任务/取消语义的一部分，不可撤销，防止「取消当前」被误点复活。
enum BatchPauseReason: Equatable {
    case none
    case afterCurrent
    case singleRun
    case stopped
}

/// 08205：串行队列调度器。
///
/// 不变量：
/// - 固定单并发（maxActive = 1）：前项 teardown（runner 返回）后才启动下一项。
/// - 每任务启动冻结 run token；stop 后 token 保持有效直到 runner 返回
///   （结果接受为 cancelled）；迟到的 progress/final/error 回调被拒绝。
/// - pauseAfterCurrent：当前任务完成后 paused（不启动下一项）；运行中可撤销
///   （clearPauseRequest），仅此一种暂停原因可撤销；resume 恢复。
/// - stop：当前任务取消 + 不再调度；等待项保持顺序；暂停原因不可撤销。
/// - 单任务 failed 默认继续下一项；failed/cancelled/interrupted 可 retry 回 waiting。
/// - remove：非活动态（waiting 与终态）；reorder/replaceConfiguration 仅 waiting。
/// - startSingle：只跑指定 waiting 任务，完成后暂停，不继续队列；暂停原因不可撤销。
/// - Scheduler 只依赖 Runner/Repository ports；不解析 IPC、不操作文件系统。
@MainActor
final class BatchQueueScheduler {
    private(set) var state: BatchQueueState

    /// 状态变化回调（08308：UI 订阅实现 live 更新；命令、终态与进度变化均触发）。
    var onStateChange: ((BatchQueueState) -> Void)?

    private let runner: any BatchTaskRunning
    private let repository: any BatchQueuePersisting

    /// 当前运行任务的 run token（nil = 无运行任务）。
    private var currentRunToken: UUID?
    private var currentTaskID: UUID?
    private var currentRunTask: Task<Void, Never>?

    /// 运行时暂停原因，不持久化。`.running` + 非 `.none` 派生为 draining 展示态。
    private(set) var pauseReason: BatchPauseReason = .none
    /// 下一次 schedule 优先这项（startSingle）；用完即清。
    private var preferredNextTaskID: UUID?

    init(runner: any BatchTaskRunning, repository: any BatchQueuePersisting, initialState: BatchQueueState = .empty) {
        self.runner = runner
        self.repository = repository
        self.state = initialState
    }

    // MARK: - 命令

    /// 开始/恢复调度（从第一个 waiting 起；paused 时恢复）。
    /// 非运行态进入才清除暂停原因：运行中调 start 不得隐式撤销暂停请求。
    func start() {
        guard state.status != .running, currentRunToken == nil else { return }
        pauseReason = .none
        state.status = .running
        scheduleNextIfPossible()
    }

    /// 完成当前项后暂停（不启动下一项）。运行中状态仍为 `.running`，须通知 UI 以显示 draining。
    /// 已处于单任务/取消暂停时保持原原因（二者已含完成后暂停，且不因本命令降级为可撤销）。
    func pauseAfterCurrent() {
        if pauseReason == .none {
            pauseReason = .afterCurrent
        }
        if currentRunToken == nil {
            state.status = .paused
            persist()
        } else {
            onStateChange?(state)
        }
    }

    /// 撤销完成后暂停：仅 `.afterCurrent` 可撤销（singleRun/stopped 不可，
    /// 避免单任务契约被软化或「取消当前」被复活）。当前任务继续，结束后仍调度下一项。
    /// 不在此处启动新任务。
    func clearPauseRequest() {
        guard state.status == .running, pauseReason == .afterCurrent else { return }
        pauseReason = .none
        onStateChange?(state)
    }

    /// 恢复调度。
    func resume() {
        start()
    }

    /// 只启动指定 waiting 任务；该项结束后暂停，不继续后续 waiting。
    @discardableResult
    func startSingle(_ taskID: UUID) -> Bool {
        guard currentRunToken == nil, state.status != .running else { return false }
        guard let index = state.tasks.firstIndex(where: { $0.id == taskID }) else { return false }
        guard BatchTaskCommandAvailability.canStartSingle(state.tasks[index].status) else { return false }
        pauseReason = .none
        preferredNextTaskID = taskID
        state.status = .running
        scheduleNextIfPossible()
        // 当前项已经占住 token 后再请求暂停，避免 schedule 入口直接 return。
        pauseReason = .singleRun
        return currentTaskID == taskID
    }

    /// 停止：取消当前任务 + 不再调度；等待项保持顺序。
    /// 当前任务的 run token 保持有效直到 runner 返回（结果被接受为 cancelled），
    /// 之后 scheduleNextIfPossible 因 `.stopped` 进入 paused；resume 前不可撤销。
    func stop() {
        pauseReason = .stopped
        currentRunTask?.cancel()
        if currentRunToken == nil {
            state.status = .paused
            persist()
        } else {
            onStateChange?(state)
        }
    }

    /// 取消指定任务：waiting 直接 cancelled；运行中请求 runner 取消；调度继续。
    @discardableResult
    func cancel(_ taskID: UUID) -> Bool {
        guard let index = state.tasks.firstIndex(where: { $0.id == taskID }) else { return false }
        let status = state.tasks[index].status
        guard BatchTaskCommandAvailability.canCancel(status) else { return false }

        if status == .waiting {
            state.tasks[index].transition(to: .cancelled)
            persist()
            return true
        }
        if currentTaskID == taskID {
            // 运行中：立即标记 cancelled（状态机拒绝后续迟到回调），并请求 runner 取消。
            if let index = state.tasks.firstIndex(where: { $0.id == taskID }) {
                _ = state.tasks[index].transition(to: .cancelled)
                persist()
            }
            currentRunTask?.cancel()
            return true
        }
        return false
    }

    /// retry：failed/cancelled/interrupted → waiting（requeue 绕过状态机终态限制）。
    @discardableResult
    func retry(_ taskID: UUID) -> Bool {
        guard let index = state.tasks.firstIndex(where: { $0.id == taskID }) else { return false }
        guard BatchTaskCommandAvailability.canRetry(state.tasks[index].status) else { return false }
        state.tasks[index].requeue()
        persist()
        return true
    }

    /// 删除任务（非活动态）。
    @discardableResult
    func remove(_ taskID: UUID) -> Bool {
        guard let index = state.tasks.firstIndex(where: { $0.id == taskID }) else { return false }
        guard BatchTaskCommandAvailability.canRemove(state.tasks[index].status) else { return false }
        state.tasks.remove(at: index)
        persist()
        return true
    }

    /// 重排（仅 waiting；参数为全部 waiting 任务的新顺序，保留各自槽位）。
    @discardableResult
    func reorder(_ orderedIDs: [UUID]) -> Bool {
        let waitingTasks = state.tasks.filter { $0.status == .waiting }
        guard waitingTasks.count == orderedIDs.count,
              Set(orderedIDs) == Set(waitingTasks.map(\.id)) else { return false }
        var byID: [UUID: BatchTask] = [:]
        for task in waitingTasks { byID[task.id] = task }
        let reorderedWaiting = orderedIDs.compactMap { byID[$0] }
        var result: [BatchTask] = []
        var waitingIndex = 0
        for task in state.tasks {
            if task.status == .waiting {
                result.append(reorderedWaiting[waitingIndex])
                waitingIndex += 1
            } else {
                result.append(task)
            }
        }
        state.tasks = result
        persist()
        return true
    }

    /// 显式替换配置（仅 waiting）。
    @discardableResult
    func replaceConfiguration(_ taskID: UUID, _ configuration: ExtractionConfiguration) -> Bool {
        guard let index = state.tasks.firstIndex(where: { $0.id == taskID }) else { return false }
        guard BatchTaskCommandAvailability.canReplaceConfiguration(state.tasks[index].status) else { return false }
        let result = state.tasks[index].replaceConfiguration(configuration)
        if result {
            persist()
        }
        return result
    }

    /// 追加等待任务（Scanner 结果；08309 接线）。
    func addTasks(_ tasks: [BatchTask]) {
        state.tasks.append(contentsOf: tasks)
        persist()
    }

    /// 设置任务的预览输出路径（waiting 或 retryable；只改 outputURL）。
    @discardableResult
    func setOutputURL(_ url: URL?, for taskID: UUID) -> Bool {
        guard let index = state.tasks.firstIndex(where: { $0.id == taskID }) else { return false }
        let taskStatus = state.tasks[index].status
        guard taskStatus == .waiting || BatchTaskCommandAvailability.canRetry(taskStatus) else { return false }
        state.tasks[index].outputURL = url
        persist()
        return true
    }

    /// 批量设置预览输出路径（避免 N 次原子保存）。
    @discardableResult
    func setOutputURLs(_ urls: [UUID: URL]) -> Bool {
        var changed = false
        for (taskID, url) in urls {
            guard let index = state.tasks.firstIndex(where: { $0.id == taskID }) else { continue }
            let taskStatus = state.tasks[index].status
            guard taskStatus == .waiting || BatchTaskCommandAvailability.canRetry(taskStatus) else { continue }
            state.tasks[index].outputURL = url
            changed = true
        }
        if changed { persist() }
        return changed
    }

    /// 设置队列级输出目的地并持久化。
    func setOutputDestination(_ destination: BatchOutputDestination) {
        state.outputDestination = destination
        persist()
    }

    // MARK: - 内部

    private func scheduleNextIfPossible() {
        guard pauseReason == .none else {
            if currentRunToken == nil {
                state.status = .paused
                persist()
            }
            return
        }
        guard currentRunToken == nil else { return }
        let nextIndex: Int
        if let preferred = preferredNextTaskID,
           let preferredIndex = state.tasks.firstIndex(where: { $0.id == preferred && $0.status == .waiting }) {
            nextIndex = preferredIndex
            preferredNextTaskID = nil
        } else if let firstWaiting = state.tasks.firstIndex(where: { $0.status == .waiting }) {
            preferredNextTaskID = nil
            nextIndex = firstWaiting
        } else {
            preferredNextTaskID = nil
            state.status = .idle
            persist()
            return
        }

        let task = state.tasks[nextIndex]
        let runToken = UUID()
        state.tasks[nextIndex].runToken = runToken
        state.tasks[nextIndex].transition(to: .preparing)
        state.runningTaskID = task.id
        currentRunToken = runToken
        currentTaskID = task.id
        persist()

        let capturedTask = state.tasks[nextIndex]
        // detached：runner 在后台执行（不阻塞 MainActor），回调回 MainActor。
        currentRunTask = Task.detached { [weak self] in
            guard let self else { return }
            let outcome = await self.runner.run(capturedTask) { [weak self] status, progress in
                Task { @MainActor in
                    self?.applyUpdate(taskID: capturedTask.id, runToken: runToken, status: status, progress: progress)
                }
            }
            await self.finishRun(taskID: capturedTask.id, runToken: runToken, outcome: outcome)
        }
    }

    /// 更新任务状态/进度（校验 run token：迟到回调被拒绝）。
    /// progress 与状态转换解耦：任务处于活动态即写入进度（同阶段多次上报保留，
    /// 状态机禁止自转换不影响 progress）；终态只走 outcome 通道（拒绝非活动态）。
    private func applyUpdate(taskID: UUID, runToken: UUID, status: BatchTaskStatus, progress: Double?) {
        guard runToken == currentRunToken, taskID == currentTaskID else { return }
        guard let index = state.tasks.firstIndex(where: { $0.id == taskID }) else { return }
        // 终态只允许通过 outcome 通道到达（finishRun）；onUpdate 仅接受活动态。
        guard BatchTaskCommandAvailability.isActive(status) else { return }
        if status != state.tasks[index].status {
            _ = state.tasks[index].transition(to: status)
        }
        // progress 仅在任务仍处于活动态时写入（cancelled/failed 后拒绝迟到进度）。
        if let progress, BatchTaskCommandAvailability.isActive(state.tasks[index].status) {
            state.tasks[index].setProgress(progress)
        }
        // live 更新：progress/状态变化不持久化（节流），但必须通知订阅者。
        onStateChange?(state)
    }

    /// 任务终态：更新状态、释放资源、调度下一项。
    private func finishRun(taskID: UUID, runToken: UUID, outcome: BatchRunOutcome) {
        guard runToken == currentRunToken, taskID == currentTaskID else { return }
        guard let index = state.tasks.firstIndex(where: { $0.id == taskID }) else {
            currentRunToken = nil
            currentTaskID = nil
            currentRunTask = nil
            return
        }

        switch outcome {
        case .completed(let entryCount, let outputURL, let runtimeIdentity):
            // 从当前活动态推进到 exporting（中间态可能已由 onUpdate 报告）。
            while let next = state.tasks[index].status.nextActiveStage {
                _ = state.tasks[index].transition(to: next)
            }
            _ = state.tasks[index].transition(to: .completed)
            state.tasks[index].recordResult(BatchTaskResult(
                entryCount: entryCount,
                outputURL: outputURL,
                runtimeIdentity: runtimeIdentity
            ))
        case .failed(let message):
            // 先记录失败（活动态守卫），再进入终态。
            state.tasks[index].recordFailure(message)
            _ = state.tasks[index].transition(to: .failed)
        case .cancelled:
            // 只有当前任务被取消（cancel/stop）才可能；保持 cancelled。
            if state.tasks[index].status == .preparing || state.tasks[index].status == .extracting
                || state.tasks[index].status == .exporting {
                _ = state.tasks[index].transition(to: .cancelled)
            }
        }

        state.runningTaskID = nil
        currentRunToken = nil
        currentTaskID = nil
        currentRunTask = nil
        persist()

        scheduleNextIfPossible()
    }

    private func persist() {
        try? repository.save(state)
        onStateChange?(state)
    }
}
