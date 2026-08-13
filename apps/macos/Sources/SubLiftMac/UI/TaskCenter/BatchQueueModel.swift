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
    /// 08511：规划失败文案（瞬时，不进 JSON / failureMessage）。
    @Published private(set) var planningErrors: [UUID: String] = [:]

    private(set) var scheduler: BatchQueueScheduler
    private let runner: any BatchTaskRunning
    private let repository: any BatchQueuePersisting
    private var pendingConflicts: [TaskCenterInteraction.OutputConflict] = []

    /// 输出冲突（开始前集中确认）——真源在 TaskCenterInteraction。
    typealias OutputConflict = TaskCenterInteraction.OutputConflict

    var outputConflicts: [OutputConflict] { pendingConflicts }

    /// 08511：按当前队列目的地构造规划器。
    private var planner: BatchOutputPlanner {
        switch state.outputDestination {
        case .sidecar:
            return BatchOutputPlanner()
        case .publicRoot(let url):
            return BatchOutputPlanner(outputRoot: url, conflictPolicy: .skip)
        }
    }

    /// 投影：搜索 + 状态筛选只改变投影，不触碰真实队列。
    var filteredTasks: [BatchTask] {
        state.tasks.filter { task in
            TaskCenterPresentation.matchesSearch(task, query: searchText)
                && statusFilter.matches(task.status)
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

    /// 只跑指定 waiting 任务。该项若在输出冲突里，返回 false，由 View 走确认。
    @discardableResult
    func startSingle(_ taskID: UUID) -> Bool {
        prepareOutputPlan()
        if planningErrors[taskID] != nil {
            syncState()
            return false
        }
        if pendingConflicts.contains(where: { $0.taskID == taskID }) {
            pendingConflicts = pendingConflicts.filter { $0.taskID == taskID }
            syncState()
            return false
        }
        let result = scheduler.startSingle(taskID)
        syncState()
        return result
    }

    /// 确认选中任务的输出冲突后只启动该项。
    func confirmOutputConflictsAndStartSingle(_ taskID: UUID) {
        if let conflict = pendingConflicts.first(where: { $0.taskID == taskID }) {
            _ = scheduler.setOutputURL(conflict.outputURL, for: taskID)
        }
        pendingConflicts = []
        _ = scheduler.startSingle(taskID)
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
        if result {
            planOutputsForWaitingTasks()
        }
        return result
    }

    @discardableResult
    func remove(_ taskID: UUID) -> Bool {
        let result = scheduler.remove(taskID)
        syncState()
        return result
    }

    /// 显式替换 waiting 任务的配置（availability 由 Scheduler 守卫；
    /// 引擎入口归一化——普通 UI 不接受 Mock，防御 API 层直调）。
    @discardableResult
    func replaceTaskConfiguration(_ taskID: UUID, _ configuration: ExtractionConfiguration) -> Bool {
        let normalized = ExtractionConfiguration(
            engine: EngineCapability.normalizedSelection(configuration.engine, developerMode: false),
            quality: configuration.quality
        )
        let result = scheduler.replaceConfiguration(taskID, normalized)
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
    /// 08511：入队后立即规划预览输出路径。
    func importInputs(_ urls: [URL]) {
        let existing = Set(state.tasks.map(\.sourceURL))
        let summary = BatchInputScanner(existingURLs: existing).scan(inputs: urls, recursive: false)
        lastScanSummary = summary
        let tasks = summary.accepted.map { item in
            BatchTask.make(
                sourceURL: item.url,
                engine: .vision,
                quality: .fast,
                developerMode: false,
                importRootURL: item.importRootURL
            )
        }
        scheduler.addTasks(tasks)
        syncState()
        planOutputsForWaitingTasks()
    }

    /// 清除扫描摘要（alert 关闭后）。
    func clearScanSummary() {
        lastScanSummary = nil
    }

    // MARK: - 08511 输出规划（导入即预览 + 开始前确认）

    /// 规划用的 sourceRoot：扫描出处。零散文件为 nil → basename。
    static func sourceRootForPlanning(_ task: BatchTask) -> URL? {
        task.importRootURL
    }

    /// 导入/改目的地后：给 waiting + retryable 任务写预览 outputURL。
    /// 同批目标碰撞先到先得；单任务失败只记 planningErrors，不阻断整批。
    func planOutputsForWaitingTasks() {
        var newErrors: [UUID: String] = [:]
        var urls: [UUID: URL] = [:]
        var seenTargets = Set<String>()
        let replannable = state.tasks.filter { $0.status == .waiting || BatchTaskCommandAvailability.canRetry($0.status) }

        for task in replannable {
            do {
                let target = try planner.previewTarget(
                    source: task.sourceURL,
                    sourceRoot: Self.sourceRootForPlanning(task)
                )
                let key = target.standardizedFileURL.path.lowercased()
                if seenTargets.contains(key) {
                    newErrors[task.id] = "目标与队列中另一任务相同"
                    continue
                }
                seenTargets.insert(key)
                urls[task.id] = target
            } catch {
                let message: String
                switch error {
                case BatchOutputPlannerError.targetOutsideRoot:
                    message = "目标在输出根之外"
                default:
                    message = "无法规划输出路径"
                }
                newErrors[task.id] = message
            }
        }

        planningErrors = newErrors
        scheduler.setOutputURLs(urls)
        syncState()
    }

    /// 预规划全部 waiting 任务的输出（开始前集中确认）：
    /// 不可写/逃逸/同批碰撞 → 收集错误、不启动；已存在目标 → pendingConflicts。
    func prepareOutputPlan() {
        pendingConflicts = []
        var newErrors: [UUID: String] = [:]
        var seenTargets = Set<String>()
        let waitingTasks = state.tasks.filter { $0.status == .waiting }

        for task in waitingTasks {
            do {
                let plan = try planner.plan(
                    source: task.sourceURL,
                    sourceRoot: Self.sourceRootForPlanning(task)
                )
                let key = plan.targetURL.standardizedFileURL.path.lowercased()
                if seenTargets.contains(key) {
                    newErrors[task.id] = "目标与队列中另一任务相同"
                    continue
                }
                seenTargets.insert(key)
                switch plan.conflict {
                case .none, .renamed, .replaced:
                    scheduler.setOutputURL(plan.targetURL, for: task.id)
                case .skipped:
                    pendingConflicts.append(OutputConflict(taskID: task.id, outputURL: plan.targetURL))
                }
            } catch {
                let message: String
                switch error {
                case BatchOutputPlannerError.targetOutsideRoot:
                    message = "目标在输出根之外"
                case BatchOutputPlannerError.unwritableDirectory:
                    message = "输出目录不可写"
                case BatchOutputPlannerError.duplicateTarget:
                    message = "目标与队列中另一任务相同"
                default:
                    message = "无法规划输出路径"
                }
                newErrors[task.id] = message
            }
        }

        planningErrors = newErrors
        syncState()
    }

    /// 确认替换冲突：冲突任务经 scheduler 写入原目标（原子替换语义=显式确认覆盖）→ 启动。
    /// 有 planningError 时 fail-closed：不启动。
    func confirmOutputConflictsAndStart() {
        guard planningErrors.isEmpty else { return }
        let urls = Dictionary(uniqueKeysWithValues: pendingConflicts.map { ($0.taskID, $0.outputURL) })
        scheduler.setOutputURLs(urls)
        pendingConflicts = []
        scheduler.start()
        syncState()
    }

    /// 取消开始：只清空冲突（不启动、不删预览 outputURL、不触碰任何文件）。
    func cancelStart() {
        pendingConflicts = []
        syncState()
    }

    /// 设置队列级输出目的地并重规划 waiting + retryable。
    func setOutputDestination(_ destination: BatchOutputDestination) {
        scheduler.setOutputDestination(destination)
        syncState()
        planOutputsForWaitingTasks()
    }

    private func syncState() {
        state = scheduler.state
    }
}
