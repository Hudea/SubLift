import Foundation

/// 08207：队列持久化错误（显式可展示）。
enum BatchQueueRepositoryError: Error, LocalizedError, Equatable, Sendable {
    /// JSON 解析失败或结构损坏（不覆盖原文件）。
    case corrupted(String)
    /// schemaVersion 不匹配（不覆盖原文件）。
    case unknownSchemaVersion(Int)

    var errorDescription: String? {
        switch self {
        case .corrupted(let detail):
            return "批量队列清单损坏：\(detail)"
        case .unknownSchemaVersion(let version):
            return "批量队列清单版本不受支持（\(version)，当前 1）"
        }
    }
}

/// 08207：BatchQueuePersisting 生产实现——versioned Codable JSON + 原子替换。
///
/// - 默认路径：Application Support/SubLift/batch-queue-v1.json。
/// - 保存：snapshot → 同目录临时文件（`.tmp-UUID`）→ 原子替换；失败原文件字节不变、临时清理。
/// - 恢复：文件缺失 → `.empty`；损坏/未知版本 fail-closed（不覆盖原文件）；
///   活动任务（preparing/extracting/exporting）→ interrupted，队列 → paused；
///   不创建 Worker（纯状态转换）。
/// - 节流：progress 不触发保存（调用方 Scheduler 只在结构/状态变化时 persist）；
///   save 始终原子全量。
final class BatchQueueRepository: BatchQueuePersisting, @unchecked Sendable {

    let fileURL: URL
    private let encoder: JSONEncoder
    private let decoder: JSONDecoder
    private let fileManager = FileManager.default

    /// 默认 Application Support 路径。
    convenience init() throws {
        let base = try FileManager.default.url(
            for: .applicationSupportDirectory,
            in: .userDomainMask,
            appropriateFor: nil,
            create: true
        )
        let dir = base.appendingPathComponent("SubLift", isDirectory: true)
        try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        self.init(fileURL: dir.appendingPathComponent("batch-queue-v1.json"))
    }

    init(fileURL: URL) {
        self.fileURL = fileURL
        let encoder = JSONEncoder()
        encoder.dateEncodingStrategy = .deferredToDate
        encoder.outputFormatting = [.sortedKeys]
        self.encoder = encoder
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .deferredToDate
        self.decoder = decoder
    }

    func load() throws -> BatchQueueState {
        guard fileManager.fileExists(atPath: fileURL.path) else {
            return .empty
        }
        let data: Data
        do {
            data = try Data(contentsOf: fileURL)
        } catch {
            throw BatchQueueRepositoryError.corrupted("读取失败：\(error.localizedDescription)")
        }

        let snapshot: BatchQueueSnapshot
        do {
            snapshot = try decoder.decode(BatchQueueSnapshot.self, from: data)
        } catch {
            throw BatchQueueRepositoryError.corrupted("解析失败：\(error.localizedDescription)")
        }

        guard snapshot.schemaVersion == BatchQueueSnapshot.currentSchemaVersion else {
            throw BatchQueueRepositoryError.unknownSchemaVersion(snapshot.schemaVersion)
        }

        // 恢复语义：活动任务 → interrupted；running → paused；不创建 Worker。
        var tasks = snapshot.tasks
        for index in tasks.indices {
            if tasks[index].status == .preparing
                || tasks[index].status == .extracting
                || tasks[index].status == .exporting {
                // fail-closed：状态机矩阵保证合法；若未来矩阵变更导致非法，报损坏。
                guard tasks[index].transition(to: .interrupted) else {
                    throw BatchQueueRepositoryError.corrupted("活动任务无法转换为 interrupted（状态机变更？）")
                }
            }
        }
        let recoveredStatus: BatchQueueStatus = snapshot.status == .running ? .paused : snapshot.status
        return BatchQueueState(
            status: recoveredStatus,
            tasks: tasks,
            runningTaskID: nil
        )
    }

    func save(_ state: BatchQueueState) throws {
        let snapshot = BatchQueueSnapshot(
            savedAt: Date(),
            tasks: state.tasks,
            runningTaskID: state.runningTaskID,
            status: state.status
        )
        let data: Data
        do {
            data = try encoder.encode(snapshot)
        } catch {
            throw BatchQueueRepositoryError.corrupted("编码失败：\(error.localizedDescription)")
        }

        let dir = fileURL.deletingLastPathComponent()
        let tmp = dir.appendingPathComponent(".\(fileURL.lastPathComponent).tmp-\(UUID().uuidString)")

        do {
            try data.write(to: tmp, options: [.atomic])
        } catch {
            try? fileManager.removeItem(at: tmp)
            throw error
        }

        var moved = false
        defer {
            if !moved {
                try? fileManager.removeItem(at: tmp)
            }
        }
        if fileManager.fileExists(atPath: fileURL.path) {
            guard let _ = try fileManager.replaceItemAt(fileURL, withItemAt: tmp) else {
                throw BatchQueueRepositoryError.corrupted("原子替换失败")
            }
        } else {
            try fileManager.moveItem(at: tmp, to: fileURL)
        }
        moved = true
    }

    /// 显式创建新清单（错误后调用方选择；删除原文件）。
    func reset() throws {
        if fileManager.fileExists(atPath: fileURL.path) {
            try fileManager.removeItem(at: fileURL)
        }
    }
}
