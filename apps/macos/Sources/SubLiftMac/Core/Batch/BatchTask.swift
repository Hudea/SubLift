import Foundation

/// 08102：批量任务状态机（纯值，无副作用）。08205 增加 nextActiveStage 辅助。
///
/// 合法转换：
/// waiting → preparing → extracting → exporting → completed
/// 活动态（preparing/extracting/exporting）可进入 failed/cancelled/interrupted；
/// waiting 可进入 cancelled/skipped/interrupted；终态不可再转换。
enum BatchTaskStatus: String, Codable, CaseIterable, Sendable {
    case waiting
    case preparing
    case extracting
    case exporting
    case completed
    case failed
    case cancelled
    case interrupted
    case skipped

    /// 活动态的主链下一阶段（preparing→extracting→exporting；其余 nil）。
    var nextActiveStage: BatchTaskStatus? {
        switch self {
        case .preparing: .extracting
        case .extracting: .exporting
        default: nil
        }
    }

    /// 纯状态机：判断 `from → to` 是否合法。
    static func canTransition(from: BatchTaskStatus, to: BatchTaskStatus) -> Bool {
        switch from {
        case .waiting:
            return to == .preparing || to == .cancelled || to == .skipped || to == .interrupted
        case .preparing:
            return to == .extracting || to == .failed || to == .cancelled || to == .interrupted
        case .extracting:
            return to == .exporting || to == .failed || to == .cancelled || to == .interrupted
        case .exporting:
            return to == .completed || to == .failed || to == .cancelled || to == .interrupted
        case .completed, .failed, .cancelled, .interrupted, .skipped:
            return false
        }
    }
}

/// 08102：任务成功结果（completed 后填充）。
struct BatchTaskResult: Codable, Equatable, Sendable {
    let entryCount: Int
    let outputURL: URL
    let runtimeIdentity: String?
}

/// 08102：批量任务值类型。
///
/// 配置在 waiting 可显式替换；preparing 及以后锁定。
/// `runToken` 是瞬态运行标识，不进入持久化值（CodingKeys 排除），
/// 且不参与相等性（持久化往返后 decode 值为 nil，仍与原任务相等）。
struct BatchTask: Identifiable, Equatable, Codable, Sendable {
    let id: UUID
    let sourceURL: URL
    var configuration: ExtractionConfiguration
    /// 输出计划目标（由 08104 OutputPlanner 填充）。
    var outputURL: URL?
    private(set) var status: BatchTaskStatus
    /// 进度 0–1（clamp）；由 Worker/Runner 按状态生命周期设置。
    private(set) var progress: Double?
    private(set) var result: BatchTaskResult?
    private(set) var failureMessage: String?
    let createdAt: Date
    /// 瞬态：运行 token 不持久化、不参与相等性。
    var runToken: UUID?

    private enum CodingKeys: String, CodingKey {
        case id, sourceURL, configuration, outputURL, status, progress, result, failureMessage, createdAt
    }

    /// 相等性排除瞬态 runToken（持久化往返保真）。
    static func == (lhs: BatchTask, rhs: BatchTask) -> Bool {
        lhs.id == rhs.id
            && lhs.sourceURL == rhs.sourceURL
            && lhs.configuration == rhs.configuration
            && lhs.outputURL == rhs.outputURL
            && lhs.status == rhs.status
            && lhs.progress == rhs.progress
            && lhs.result == rhs.result
            && lhs.failureMessage == rhs.failureMessage
            && lhs.createdAt == rhs.createdAt
    }

    /// 创建任务（waiting）。引擎经归一化入口处理：
    /// 非开发模式的隐藏 Mock 回落 Vision（与提取请求路径同一策略）。
    /// `sourceURL` 契约：绝对 file URL（标准化由 08103 Scanner 负责）。
    static func make(
        id: UUID = UUID(),
        sourceURL: URL,
        engine: OcrEngineName,
        quality: SamplingQuality,
        developerMode: Bool,
        createdAt: Date = Date()
    ) -> BatchTask {
        BatchTask(
            id: id,
            sourceURL: sourceURL,
            configuration: ExtractionConfiguration(
                engine: EngineCapability.normalizedSelection(engine, developerMode: developerMode),
                quality: quality
            ),
            status: .waiting,
            createdAt: createdAt
        )
    }

    /// 合法状态转换；非法返回 false 且不改变状态。
    @discardableResult
    mutating func transition(to newStatus: BatchTaskStatus) -> Bool {
        guard BatchTaskStatus.canTransition(from: status, to: newStatus) else { return false }
        status = newStatus
        return true
    }

    /// 显式替换配置；仅 waiting 可改（preparing 及以后锁定）。
    @discardableResult
    mutating func replaceConfiguration(_ newConfiguration: ExtractionConfiguration) -> Bool {
        guard status == .waiting else { return false }
        configuration = newConfiguration
        return true
    }

    /// retry 专用：failed/cancelled/interrupted → waiting（绕过状态机终态限制），
    /// 清除瞬态 run token 与上一轮失败元数据（重新排队语义）。方法内守卫防止非法 requeue。
    mutating func requeue() {
        guard BatchTaskCommandAvailability.canRetry(status) else { return }
        status = .waiting
        runToken = nil
        failureMessage = nil
        progress = nil
        result = nil
    }

    /// 设置进度（clamp 到 0–1）。
    mutating func setProgress(_ value: Double) {
        progress = min(1, max(0, value))
    }

    /// 记录失败摘要（活动态守卫：仅 preparing/extracting/exporting 可写）。
    mutating func recordFailure(_ message: String) {
        guard status == .preparing || status == .extracting || status == .exporting else { return }
        failureMessage = message
    }

    /// 记录成功结果（仅 completed 可写）。
    mutating func recordResult(_ result: BatchTaskResult) {
        guard status == .completed else { return }
        self.result = result
    }
}
