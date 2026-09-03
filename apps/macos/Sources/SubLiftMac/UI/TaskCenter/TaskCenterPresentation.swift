import Foundation

/// 08511：表格列枚举——用于 visibleColumns 四档宽度。
enum TaskTableColumn: Equatable, CaseIterable {
    case file, location, status, progress, engine, duration, output, added
}

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

    /// 08511：空画布判定——队列无任务时展示 drop zone（筛选结果为空 ≠ 空态）。
    static func shouldShowEmptyCanvas(for queue: BatchQueueState) -> Bool {
        queue.tasks.isEmpty
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

    // MARK: - 08511 位置列与搜索

    /// 四档列宽：文件/状态/进度 → +输出 → +位置 → +引擎/时长/添加时间。
    static func visibleColumns(forWidth width: CGFloat) -> [TaskTableColumn] {
        var cols: [TaskTableColumn] = [.file, .status, .progress]
        if width >= 960 { cols.append(.output) }
        if width >= 1100 { cols.insert(.location, at: 1) }
        if width >= 1280 { cols.append(contentsOf: [.engine, .duration, .added]) }
        return cols
    }

    /// 位置列显示规则（末尾 `/` 表示从文件夹导入）。
    static func locationDisplay(for task: BatchTask) -> String {
        guard let importRoot = task.importRootURL else {
            return task.sourceURL.deletingLastPathComponent().lastPathComponent
        }
        // deletingLastPathComponent() 会加尾部 /，需要统一 trim 后再比较。
        let sourceDirPath = task.sourceURL.deletingLastPathComponent().standardizedFileURL.path.trimmingCharacters(in: CharacterSet(charactersIn: "/"))
        let rootPath = importRoot.standardizedFileURL.path.trimmingCharacters(in: CharacterSet(charactersIn: "/"))
        if sourceDirPath == rootPath {
            return importRoot.lastPathComponent + "/"
        }
        if let rel = BatchPath.relativePath(
            from: importRoot.standardizedFileURL,
            to: task.sourceURL.deletingLastPathComponent().standardizedFileURL
        ), !rel.isEmpty {
            return rel.hasSuffix("/") ? rel : rel + "/"
        }
        return task.sourceURL.deletingLastPathComponent().lastPathComponent
    }

    /// 位置列完整路径（tooltip / AX）。
    static func locationFullPath(for task: BatchTask) -> String {
        task.sourceURL.deletingLastPathComponent().standardizedFileURL.path
    }

    /// 位置列 tooltip 文案。
    static func locationTooltip(for task: BatchTask) -> String {
        if task.importRootURL != nil {
            return "从「\(task.importRootURL!.lastPathComponent)」导入 · \(locationFullPath(for: task))"
        }
        return "所在文件夹 · \(locationFullPath(for: task))"
    }

    /// 搜索：匹配文件名 + 位置展示串 + 完整目录 path。
    static func matchesSearch(_ task: BatchTask, query: String) -> Bool {
        guard !query.isEmpty else { return true }
        let q = query.localizedLowercase
        return task.sourceURL.lastPathComponent.localizedCaseInsensitiveContains(q)
            || locationDisplay(for: task).localizedCaseInsensitiveContains(q)
            || locationFullPath(for: task).localizedCaseInsensitiveContains(q)
    }

    // MARK: - 08511 Inspector 模型

    struct TaskInspectorModel: Equatable {
        let filename: String
        let statusName: String
        let locationDisplay: String
        let locationFullPath: String
        let outputFolderDisplay: String
        let outputFilename: String
        let outputFullPath: String?
        let outputFileExists: Bool
        let planningError: String?
        let outputExistsWarning: String?
        let engineDisplay: String
        let qualityDisplay: String
        let canEditConfiguration: Bool
        let failureMessage: String?
        let runtimeIdentity: String?
        let canCancel: Bool
        let canRetry: Bool
        let canRemove: Bool
        let canReorder: Bool
        let canStartSingle: Bool
    }

    static func inspectorModel(
        for task: BatchTask,
        fileExists: Bool,
        planningError: String?
    ) -> TaskInspectorModel {
        let outputURL = task.outputURL
        let outputExists = fileExists && outputURL != nil
        return TaskInspectorModel(
            filename: task.sourceURL.lastPathComponent,
            statusName: statusDisplayName(task.status),
            locationDisplay: locationDisplay(for: task),
            locationFullPath: locationFullPath(for: task),
            outputFolderDisplay: outputURL?.deletingLastPathComponent().lastPathComponent ?? "—",
            outputFilename: outputURL?.lastPathComponent ?? "—",
            outputFullPath: outputURL?.standardizedFileURL.path,
            outputFileExists: outputExists,
            planningError: planningError,
            outputExistsWarning: outputExists ? "该字幕已存在，开始时将确认是否替换" : nil,
            engineDisplay: task.configuration.engine.displayName,
            qualityDisplay: qualityDisplayName(task.configuration.quality),
            canEditConfiguration: BatchTaskCommandAvailability.canReplaceConfiguration(task.status),
            failureMessage: task.failureMessage,
            runtimeIdentity: task.result?.runtimeIdentity,
            canCancel: BatchTaskCommandAvailability.canCancel(task.status),
            canRetry: BatchTaskCommandAvailability.canRetry(task.status),
            canRemove: BatchTaskCommandAvailability.canRemove(task.status),
            canReorder: BatchTaskCommandAvailability.canReorder(task.status),
            canStartSingle: BatchTaskCommandAvailability.canStartSingle(task.status)
        )
    }

    static func statusSymbolName(_ status: BatchTaskStatus) -> String {
        switch status {
        case .waiting: "circle"
        case .preparing, .extracting, .exporting: "ellipsis.circle"
        case .completed: "checkmark.circle.fill"
        case .failed: "xmark.circle.fill"
        case .cancelled: "minus.circle"
        case .interrupted: "pause.circle"
        case .skipped: "forward.circle"
        }
    }

    static func canStartSingle(_ queue: BatchQueueState, taskID: UUID) -> Bool {
        queue.status != .running
            && queue.tasks.contains { $0.id == taskID && BatchTaskCommandAvailability.canStartSingle($0.status) }
    }

    // MARK: - 队列级工具栏投影（display 派生，不持久化）

    enum QueueDisplayState: Equatable {
        case idle, running, draining, paused
    }

    enum QueuePrimaryAction: Equatable {
        case start, pauseAfterCurrent, clearPauseRequest, resume, none
    }

    struct QueueTransport: Equatable {
        let displayState: QueueDisplayState
        let primaryAction: QueuePrimaryAction
        let primaryTitle: String
        let primarySymbolName: String
        let primaryHelp: String
        let primaryEnabled: Bool
        let primaryIsPressed: Bool
        let showsCancelCurrent: Bool
        let statusText: String
    }

    static func displayState(for queue: BatchQueueState, reason: BatchPauseReason) -> QueueDisplayState {
        switch queue.status {
        case .idle:
            return .idle
        case .running:
            return reason == .none ? .running : .draining
        case .paused, .interrupted:
            return .paused
        }
    }

    static func transport(for queue: BatchQueueState, reason: BatchPauseReason) -> QueueTransport {
        let display = displayState(for: queue, reason: reason)
        let hasWaiting = queue.pendingTasks.contains { $0.status == .waiting }
        let currentFilename = queue.activeTasks.first?.sourceURL.lastPathComponent

        switch display {
        case .idle:
            return QueueTransport(
                displayState: .idle,
                primaryAction: hasWaiting ? .start : .none,
                primaryTitle: "开始队列",
                primarySymbolName: "play.fill",
                primaryHelp: "开始按顺序提取等待中的任务",
                primaryEnabled: hasWaiting,
                primaryIsPressed: false,
                showsCancelCurrent: false,
                statusText: queue.tasks.isEmpty ? "" : "队列空闲"
            )
        case .running:
            return QueueTransport(
                displayState: .running,
                primaryAction: .pauseAfterCurrent,
                primaryTitle: "完成后暂停",
                primarySymbolName: "pause.fill",
                primaryHelp: "当前任务完成后暂停队列，不中断正在提取的任务",
                primaryEnabled: true,
                primaryIsPressed: false,
                showsCancelCurrent: true,
                statusText: runningStatusText("队列运行中", filename: currentFilename)
            )
        case .draining:
            // draining 按暂停原因分三态：仅 afterCurrent 可点击撤销。
            switch reason {
            case .stopped:
                return QueueTransport(
                    displayState: .draining,
                    primaryAction: .none,
                    primaryTitle: "继续队列",
                    primarySymbolName: "play.fill",
                    primaryHelp: "正在取消当前任务，完成后队列暂停，届时可继续",
                    primaryEnabled: false,
                    primaryIsPressed: false,
                    showsCancelCurrent: false,
                    statusText: runningStatusText("正在取消当前任务", filename: currentFilename)
                )
            case .singleRun:
                return QueueTransport(
                    displayState: .draining,
                    primaryAction: .none,
                    primaryTitle: "完成后暂停",
                    primarySymbolName: "pause.fill",
                    primaryHelp: "单独运行该任务，完成后暂停队列",
                    primaryEnabled: false,
                    primaryIsPressed: true,
                    showsCancelCurrent: true,
                    statusText: runningStatusText("单独运行 · 完成后将暂停", filename: currentFilename)
                )
            case .afterCurrent, .none:
                return QueueTransport(
                    displayState: .draining,
                    primaryAction: .clearPauseRequest,
                    primaryTitle: "完成后暂停",
                    primarySymbolName: "pause.fill",
                    primaryHelp: "将在当前任务完成后暂停队列，再次点击可继续调度",
                    primaryEnabled: true,
                    primaryIsPressed: true,
                    showsCancelCurrent: true,
                    statusText: runningStatusText("完成后将暂停", filename: currentFilename)
                )
            }
        case .paused:
            let waitingCount = queue.pendingTasks.count
            let statusText = waitingCount > 0
                ? "队列已暂停 · 还剩 \(waitingCount) 项"
                : "队列已暂停"
            return QueueTransport(
                displayState: .paused,
                primaryAction: hasWaiting ? .resume : .none,
                primaryTitle: "继续队列",
                primarySymbolName: "play.fill",
                primaryHelp: "继续提取剩余等待中的任务",
                primaryEnabled: hasWaiting,
                primaryIsPressed: false,
                showsCancelCurrent: false,
                statusText: statusText
            )
        }
    }

    private static func runningStatusText(_ prefix: String, filename: String?) -> String {
        if let filename, !filename.isEmpty {
            return "\(prefix) · 当前 \(filename)"
        }
        return prefix
    }
}
