import Foundation

/// 08308：Task Center 展示纯逻辑（真实字段投影，不编造）。
enum TaskCenterPresentation {

    // MARK: - 汇总

    struct Summary: Equatable {
        let total: Int
        let waiting: Int
        let active: Int
        let completed: Int
        let failed: Int
        let cancelled: Int
        let skipped: Int
    }

    static func summary(for queue: BatchQueueState) -> Summary {
        Summary(
            total: queue.tasks.count,
            waiting: queue.tasks.filter { $0.status == .waiting }.count,
            active: queue.activeTasks.count,
            completed: queue.tasks.filter { $0.status == .completed }.count,
            failed: queue.tasks.filter { $0.status == .failed }.count,
            cancelled: queue.tasks.filter { $0.status == .cancelled }.count,
            skipped: queue.tasks.filter { $0.status == .skipped }.count
        )
    }

    // MARK: - 状态投影

    static func statusDisplayName(_ status: BatchTaskStatus) -> String {
        switch status {
        case .waiting: "等待中"
        case .preparing: "准备中"
        case .extracting: "提取中"
        case .exporting: "导出中"
        case .completed: "已完成"
        case .failed: "失败"
        case .cancelled: "已取消"
        case .interrupted: "已中断"
        case .skipped: "已跳过"
        }
    }

    /// 进度文本：仅活动态且有真实 progress 时显示；否则 nil（不编造）。
    static func progressText(for task: BatchTask) -> String? {
        guard task.status == .preparing || task.status == .extracting || task.status == .exporting,
              let progress = task.progress else { return nil }
        return "\(Int((progress * 100).rounded()))%"
    }

    static func engineDisplayName(for task: BatchTask) -> String {
        task.configuration.engine.displayName
    }

    /// 时长列：任务模型无真实时长字段——显示占位（不编造、不用条目数冒充）。
    static func durationText(for task: BatchTask) -> String {
        "—"
    }

    // MARK: - 详情行

    struct DetailRow: Equatable {
        let label: String
        let value: String
    }

    static func detailRows(for task: BatchTask) -> [DetailRow] {
        var rows: [DetailRow] = []
        rows.append(DetailRow(label: "文件", value: task.sourceURL.lastPathComponent))
        rows.append(DetailRow(label: "路径", value: task.sourceURL.standardizedFileURL.path))
        rows.append(DetailRow(label: "配置", value: "\(task.configuration.engine.displayName) / \(qualityDisplayName(task.configuration.quality))"))
        if let runtime = task.result?.runtimeIdentity {
            rows.append(DetailRow(label: "Runtime", value: runtime))
        }
        if let message = task.failureMessage {
            rows.append(DetailRow(label: "错误", value: message))
        }
        if let output = task.outputURL {
            rows.append(DetailRow(label: "输出", value: output.standardizedFileURL.path))
        }
        rows.append(DetailRow(label: "Task ID", value: task.id.uuidString))
        return rows
    }

    static func qualityDisplayName(_ quality: SamplingQuality) -> String {
        switch quality {
        case .fast: "快速"
        case .balanced: "平衡"
        case .fine: "精细"
        }
    }

    // MARK: - 队列级命令 availability

    static func canStart(_ queue: BatchQueueState) -> Bool {
        queue.status != .running && queue.pendingTasks.contains(where: { $0.status == .waiting })
    }

    static func canPauseAfterCurrent(_ queue: BatchQueueState) -> Bool {
        queue.status == .running
    }

    static func canResume(_ queue: BatchQueueState) -> Bool {
        queue.status == .paused && queue.pendingTasks.contains(where: { $0.status == .waiting })
    }

    static func canStop(_ queue: BatchQueueState) -> Bool {
        queue.status == .running
    }
}
