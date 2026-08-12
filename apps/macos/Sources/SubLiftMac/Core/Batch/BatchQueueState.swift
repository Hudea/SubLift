import Foundation

/// 08102：队列级状态。
enum BatchQueueStatus: String, Codable, Sendable {
    case idle
    case running
    case paused
    case interrupted
}

/// 08102：批量队列值类型（可持久化）。
///
/// 08207 恢复语义：持久化恢复时活动任务（preparing/extracting/exporting）
/// 转为 interrupted、队列转为 paused，不自动启动 Worker。
struct BatchQueueState: Codable, Equatable, Sendable {
    var status: BatchQueueStatus
    var tasks: [BatchTask]
    var runningTaskID: UUID?

    static let empty = BatchQueueState(status: .idle, tasks: [], runningTaskID: nil)

    /// 活动任务（preparing/extracting/exporting）。
    var activeTasks: [BatchTask] {
        tasks.filter { $0.status == .preparing || $0.status == .extracting || $0.status == .exporting }
    }

    /// 待处理任务（waiting）。
    var pendingTasks: [BatchTask] {
        tasks.filter { $0.status == .waiting }
    }
}
