import Foundation

/// 08309：Task Center 批量交互纯逻辑（多选删除/重排/替换确认/Finder）。
enum TaskCenterInteraction {

    /// 输出冲突（开始前集中确认）——单一真源（Model 与 View 共用）。
    struct OutputConflict: Equatable {
        let taskID: UUID
        let outputURL: URL
    }

    enum ReplaceDecision: Equatable {
        case replaceAndStart
        case cancel
    }

    enum MoveDirection {
        case up, down
    }

    /// 仅 waiting 任务可批量删除（availability 唯一真源）。
    static func removableTaskIDs(from tasks: [BatchTask]) -> Set<UUID> {
        Set(tasks.filter { BatchTaskCommandAvailability.canRemove($0.status) }.map(\.id))
    }

    /// 重排（仅 waiting；相邻 waiting 槽位交换，非 waiting 槽位不动）。
    static func moveTask(id: UUID, in tasks: inout [BatchTask], direction: MoveDirection) {
        guard let index = tasks.firstIndex(where: { $0.id == id }) else { return }
        guard BatchTaskCommandAvailability.canReorder(tasks[index].status) else { return }
        let waitingIndices = tasks.indices.filter { tasks[$0].status == .waiting }
        guard let currentPosition = waitingIndices.firstIndex(of: index) else { return }
        let targetPosition: Int
        switch direction {
        case .up:
            guard currentPosition > 0 else { return }
            targetPosition = currentPosition - 1
        case .down:
            guard currentPosition < waitingIndices.count - 1 else { return }
            targetPosition = currentPosition + 1
        }
        tasks.swapAt(index, waitingIndices[targetPosition])
    }

    /// 替换确认决策（取消不启动、不删除/覆盖任何输出）——requestStart 消费。
    static func replaceDecision(confirming: Bool, conflicts: [OutputConflict]) -> ReplaceDecision {
        confirming ? .replaceAndStart : .cancel
    }

    /// Finder 定位候选：仅存在的文件可定位（不存在返回 nil → 错误提示）。
    static func revealCandidate(for url: URL) -> URL? {
        FileManager.default.fileExists(atPath: url.path) ? url : nil
    }
}

/// 08309：Task Center 无障碍纯逻辑（AX label/value 组合；状态不只依赖颜色）。
enum TaskCenterAccessibility {

    /// 状态 AX label：状态文本（颜色之外的语义）。
    static func statusLabel(for status: BatchTaskStatus) -> String {
        TaskCenterPresentation.statusDisplayName(status)
    }

    /// 进度 AX label：真实 progress 才有（不编造）。
    static func progressLabel(for task: BatchTask) -> String? {
        guard let text = TaskCenterPresentation.progressText(for: task) else { return nil }
        return "进度 \(text)"
    }

    /// 汇总条 AX 组合 label。
    static func summaryLabel(
        total: Int, waiting: Int, active: Int, completed: Int,
        failed: Int, cancelled: Int, skipped: Int
    ) -> String {
        "总数 \(total)，等待 \(waiting)，进行中 \(active)，完成 \(completed)，失败 \(failed)，取消 \(cancelled)，跳过 \(skipped)"
    }
}
