import SwiftUI
import Foundation

/// 08308：App scene 层唯一的批量队列模型（包装 Scheduler + Repository）。
///
/// - 启动恢复：repository.load() 成功 → scheduler(initialState)；
///   损坏/未知版本 → 可展示错误 + 空队列（不覆盖原文件）。
/// - 转发命令到 Scheduler（不复制业务逻辑）。
@MainActor
final class BatchQueueModel: ObservableObject {
    @Published private(set) var state: BatchQueueState
    @Published private(set) var recoveryErrorMessage: String?

    private(set) var scheduler: BatchQueueScheduler
    private let runner: any BatchTaskRunning
    private let repository: any BatchQueuePersisting

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

    @discardableResult
    func replaceConfiguration(_ taskID: UUID, _ configuration: ExtractionConfiguration) -> Bool {
        let result = scheduler.replaceConfiguration(taskID, configuration)
        syncState()
        return result
    }

    /// 追加等待任务（Scanner 结果）。
    func addTasks(_ tasks: [BatchTask]) {
        scheduler.addTasks(tasks)
        syncState()
    }

    private func syncState() {
        state = scheduler.state
    }
}
