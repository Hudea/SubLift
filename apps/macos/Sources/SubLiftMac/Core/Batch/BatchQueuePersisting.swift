import Foundation

/// 08205：Repository port——队列持久化抽象。
///
/// 08207 实现 JSON 持久化（Application Support/SubLift/batch-queue-v1.json，
/// schemaVersion=1，原子替换）；测试用内存假实现。
protocol BatchQueuePersisting: AnyObject, Sendable {
    func load() throws -> BatchQueueState
    func save(_ state: BatchQueueState) throws
}
