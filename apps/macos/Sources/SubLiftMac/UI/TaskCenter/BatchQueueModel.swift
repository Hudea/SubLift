import SwiftUI
import Foundation

/// 08309：状态筛选（纯投影过滤器）。
enum BatchTaskStatusFilter: String, CaseIterable, Sendable {
    case all, waiting, running, completed, failed, cancelled, skipped

    func matches(_ status: BatchTaskStatus) -> Bool {
        switch self {
        case .all: true
        case .waiting: status == .waiting
        case .running: status == .preparing || status == .extracting || status == .exporting
        case .completed: status == .completed
        case .failed: status == .failed
        case .cancelled: status == .cancelled
        case .skipped: status == .skipped
        }
    }
}

/// 08308：App scene 层唯一的批量队列模型（包装 Scheduler + Repository）。
///
/// - 启动恢复：repository.load() 成功 → scheduler(initialState)；
///   损坏/未知版本 → 可展示错误 + 空队列（不覆盖原文件）。
/// - 转发命令到 Scheduler（不复制业务逻辑）。
/// - 08309：统一导入（scanner 接线）、投影（搜索/筛选）、输出规划（开始前集中确认）。
@MainActor
final class BatchQueueModel: ObservableObject {
    @Published private(set) var state: BatchQueueState
    @Published private(set) var recoveryErrorMessage: String?

    // MARK: 08309 投影与导入状态

    @Published var searchText: String = ""
    @Published var statusFilter: BatchTaskStatusFilter = .all
    @Published private(set) var lastScanSummary: BatchScanSummary?

    private(set) var scheduler: BatchQueueScheduler
    private let runner: any BatchTaskRunning
    private let repository: any BatchQueuePersisting
    private let planner = BatchOutputPlanner()
    private var pendingConflicts: [TaskCenterInteraction.OutputConflict] = []

    /// 输出冲突（开始前集中确认）——真源在 TaskCenterInteraction。
    typealias OutputConflict = TaskCenterInteraction.OutputConflict

    var outputConflicts: [OutputConflict] { pendingConflicts }

    /// 投影：搜索 + 状态筛选只改变投影，不触碰真实队列。
    var filteredTasks: [BatchTask] {
        state.tasks.filter { task in
            let matchesSearch = searchText.isEmpty
                || task.sourceURL.lastPathComponent.localizedCaseInsensitiveContains(searchText)
            return matchesSearch && statusFilter.matches(task.status)
        }
    }

    init(
        runner: any BatchTaskRunning,
        repository: any BatchQueuePersisting,
        initialState: BatchQueueState? = nil
    ) {
        self.runner = runner
        self.repository = repository

        // 先解析初始状态（避免在存储属性未全部初始化前访问 self）。
        var resolved: BatchQueueState
        var recoveryError: String?
        if let initialState {
            resolved = initialState
        } else {
            do {
                resolved = try repository.load()
            } catch {
                // fail-closed：损坏/未知版本不覆盖原文件，空队列 + 可展示错误。
                resolved = .empty
                recoveryError = (error as? LocalizedError)?.errorDescription ?? "\(error)"
            }
        }
        self.state = resolved
        self.recoveryErrorMessage = recoveryError
        self.scheduler = BatchQueueScheduler(
            runner: runner,
            repository: repository,
            initialState: resolved
        )
        self.state = self.scheduler.state
        // live 更新：scheduler 异步变更（进度/终态/调度）通过回调同步到 @Published state。
        self.scheduler.onStateChange = { [weak self] newState in
            self?.state = newState
        }
    }

    /// 消解恢复错误提示（alert 用）。
    func dismissRecoveryError() {
        recoveryErrorMessage = nil
    }

    #if DEBUG
    /// DEBUG-only：确定性 fixture 状态注入（EvidenceShot 截图用；不冒充真实 OCR）。
    func installFixture(_ fixture: BatchQueueState) {
        scheduler = BatchQueueScheduler(
            runner: runner,
            repository: repository,
            initialState: fixture
        )
        scheduler.onStateChange = { [weak self] newState in
            self?.state = newState
        }
        state = scheduler.state
        recoveryErrorMessage = nil
    }
    #endif
    // MARK: - Scheduler 命令转发

    func start() {
        scheduler.start()
        syncState()
    }

    func pauseAfterCurrent() {
        scheduler.pauseAfterCurrent()
        syncState()
    }

    func resume() {
        scheduler.resume()
        syncState()
    }

    func stop() {
        scheduler.stop()
        syncState()
    }

    @discardableResult
    func cancel(_ taskID: UUID) -> Bool {
        let result = scheduler.cancel(taskID)
        syncState()
        return result
    }

    @discardableResult
    func retry(_ taskID: UUID) -> Bool {
        let result = scheduler.retry(taskID)
        syncState()
        return result
    }

    @discardableResult
    func remove(_ taskID: UUID) -> Bool {
        let result = scheduler.remove(taskID)
        syncState()
        return result
    }

    /// 显式替换 waiting 任务的配置（availability 由 Scheduler 守卫）。
    @discardableResult
    func replaceTaskConfiguration(_ taskID: UUID, _ configuration: ExtractionConfiguration) -> Bool {
        let result = scheduler.replaceConfiguration(taskID, configuration)
        syncState()
        return result
    }

    /// 追加等待任务（Scanner 结果）。
    func addTasks(_ tasks: [BatchTask]) {
        scheduler.addTasks(tasks)
        syncState()
    }

    // MARK: - 08309 统一导入（文件/文件夹/drop 同一 scanner）

    /// 统一导入入口：全部走 BatchInputScanner（existingURLs 取当前队列，批间去重）——
    /// accepted 构造任务入队，rejected 记录到 lastScanSummary（B02 摘要），不自动开始。
    func importInputs(_ urls: [URL]) {
        let existing = Set(state.tasks.map(\.sourceURL))
        let summary = BatchInputScanner(existingURLs: existing).scan(inputs: urls, recursive: false)
        lastScanSummary = summary
        let tasks = summary.accepted.map { url in
            BatchTask.make(
                sourceURL: url,
                engine: .vision,
                quality: .fast,
                developerMode: false
            )
        }
        scheduler.addTasks(tasks)
        syncState()
    }

    /// 清除扫描摘要（alert 关闭后）。
    func clearScanSummary() {
        lastScanSummary = nil
    }

    // MARK: - 08309 输出规划（开始前集中确认）

    /// 预规划全部 waiting 任务的输出（sidecar 默认 + skip 策略）：
    /// 无冲突任务经 scheduler 写入 outputURL；目标已存在 → pendingConflicts（集中确认）。
    func prepareOutputPlan() {
        pendingConflicts = []
        let waitingTasks = state.tasks.filter { $0.status == .waiting }
        for task in waitingTasks {
            guard let plan = try? planner.plan(source: task.sourceURL, sourceRoot: nil) else { continue }
            switch plan.conflict {
            case .none, .renamed, .replaced:
                scheduler.setOutputURL(plan.targetURL, for: task.id)
            case .skipped:
                pendingConflicts.append(OutputConflict(taskID: task.id, outputURL: plan.targetURL))
            }
        }
        syncState()
    }

    /// 确认替换冲突：冲突任务经 scheduler 写入原目标（原子替换语义=显式确认覆盖）→ 启动。
    func confirmOutputConflictsAndStart() {
        for conflict in pendingConflicts {
            scheduler.setOutputURL(conflict.outputURL, for: conflict.taskID)
        }
        pendingConflicts = []
        scheduler.start()
        syncState()
    }

    /// 取消开始：清空冲突与已规划的输出（不启动、不删除/覆盖任何输出）。
    func cancelStart() {
        pendingConflicts = []
        for task in state.tasks where task.status == .waiting {
            scheduler.setOutputURL(nil, for: task.id)
        }
        syncState()
    }

    private func syncState() {
        state = scheduler.state
    }
}
