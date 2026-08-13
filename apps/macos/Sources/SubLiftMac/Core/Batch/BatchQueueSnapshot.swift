import Foundation

/// 08207：队列持久化快照（schemaVersion=1）。
///
/// 保存任务摘要/顺序/配置/输出/状态/计数/错误/时间；
/// 不保存帧、字幕全文、无限日志、run token（BatchTask CodingKeys 已排除）。
struct BatchQueueSnapshot: Codable, Equatable, Sendable {
    let schemaVersion: Int
    let savedAt: Date
    let tasks: [BatchTask]
    let runningTaskID: UUID?
    let status: BatchQueueStatus
    let outputDestination: BatchOutputDestination?

    static let currentSchemaVersion = 1

    init(
        schemaVersion: Int = BatchQueueSnapshot.currentSchemaVersion,
        savedAt: Date = Date(),
        tasks: [BatchTask],
        runningTaskID: UUID?,
        status: BatchQueueStatus,
        outputDestination: BatchOutputDestination? = nil
    ) {
        self.schemaVersion = schemaVersion
        self.savedAt = savedAt
        self.tasks = tasks
        self.runningTaskID = runningTaskID
        self.status = status
        self.outputDestination = outputDestination
    }
}
