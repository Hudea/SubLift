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
/// 08511：增加队列级 outputDestination（默认 sidecar）。
struct BatchQueueState: Codable, Equatable, Sendable {
    var status: BatchQueueStatus
    var tasks: [BatchTask]
    var runningTaskID: UUID?
    var outputDestination: BatchOutputDestination

    static let empty = BatchQueueState(status: .idle, tasks: [], runningTaskID: nil, outputDestination: .sidecar)

    private enum CodingKeys: String, CodingKey {
        case status, tasks, runningTaskID, outputDestination
    }

    init(status: BatchQueueStatus, tasks: [BatchTask], runningTaskID: UUID?, outputDestination: BatchOutputDestination = .sidecar) {
        self.status = status
        self.tasks = tasks
        self.runningTaskID = runningTaskID
        self.outputDestination = outputDestination
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        status = try c.decode(BatchQueueStatus.self, forKey: .status)
        tasks = try c.decode([BatchTask].self, forKey: .tasks)
        runningTaskID = try c.decodeIfPresent(UUID.self, forKey: .runningTaskID)
        outputDestination = try c.decodeIfPresent(BatchOutputDestination.self, forKey: .outputDestination) ?? .sidecar
    }

    /// 活动任务（preparing/extracting/exporting）。
    var activeTasks: [BatchTask] {
        tasks.filter { $0.status == .preparing || $0.status == .extracting || $0.status == .exporting }
    }

    /// 待处理任务（waiting）。
    var pendingTasks: [BatchTask] {
        tasks.filter { $0.status == .waiting }
    }
}
