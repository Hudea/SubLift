import Foundation

/// 08205：单任务运行结果。
enum BatchRunOutcome: Equatable, Sendable {
    case completed(entryCount: Int, outputURL: URL, runtimeIdentity: String?)
    case failed(String)
    case cancelled
}

/// 08205：Runner port——调度器唯一的运行依赖。
///
/// Runner 负责真实 Worker/IPC（08206 实现）；调度器不解析 IPC 消息、
/// 不直接操作文件系统。
protocol BatchTaskRunning: AnyObject, Sendable {
    /// 运行任务直到终态。`onUpdate` 报告状态/进度（调度器据此更新任务）。
    /// 实现应响应 Task 取消（返回 `.cancelled`）。
    func run(
        _ task: BatchTask,
        onUpdate: @escaping @Sendable (BatchTaskStatus, Double?) -> Void
    ) async -> BatchRunOutcome
}
