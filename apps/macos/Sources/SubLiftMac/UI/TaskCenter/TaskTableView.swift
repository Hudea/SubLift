import SwiftUI

/// 08308/08309：任务表格（原生 Table；多选）。
/// 08511：四档 Table 变体，不把隐藏列留成 width 0。
struct TaskTableView: View {
    let tasks: [BatchTask]
    @Binding var selection: Set<UUID>
    let visibleColumns: [TaskTableColumn]

    var body: some View {
        let hasLocation = visibleColumns.contains(.location)
        let hasOutput = visibleColumns.contains(.output)
        let hasFull = visibleColumns.contains(.engine)
        if hasFull {
            fullTable
        } else if hasLocation {
            wideTable
        } else if hasOutput {
            mediumTable
        } else {
            narrowTable
        }
    }

    private var narrowTable: some View {
        Table(tasks, selection: $selection) {
            TableColumn("文件") { task in
                fileCell(task, foldLocation: true)
            }
            .width(min: 140, ideal: 220)
            TableColumn("状态") { task in
                statusCell(task)
            }
            .width(min: 56, ideal: 72)
            TableColumn("进度") { task in
                progressCell(task)
            }
            .width(min: 44, ideal: 56)
        }
    }

    private var mediumTable: some View {
        Table(tasks, selection: $selection) {
            TableColumn("文件") { task in
                fileCell(task, foldLocation: true)
            }
            .width(min: 140, ideal: 220)
            TableColumn("状态") { task in
                statusCell(task)
            }
            .width(min: 56, ideal: 72)
            TableColumn("进度") { task in
                progressCell(task)
            }
            .width(min: 44, ideal: 56)
            TableColumn("输出") { task in
                outputCell(task)
            }
            .width(min: 80, ideal: 140)
        }
    }

    private var wideTable: some View {
        Table(tasks, selection: $selection) {
            TableColumn("文件") { task in
                fileCell(task, foldLocation: false)
            }
            .width(min: 140, ideal: 220)
            TableColumn("位置") { task in
                locationCell(task)
            }
            .width(min: 80, ideal: 120)
            TableColumn("状态") { task in
                statusCell(task)
            }
            .width(min: 56, ideal: 72)
            TableColumn("进度") { task in
                progressCell(task)
            }
            .width(min: 44, ideal: 56)
            TableColumn("输出") { task in
                outputCell(task)
            }
            .width(min: 80, ideal: 140)
        }
    }

    private var fullTable: some View {
        Table(tasks, selection: $selection) {
            TableColumn("文件") { task in
                fileCell(task, foldLocation: false)
            }
            .width(min: 140, ideal: 220)
            TableColumn("位置") { task in
                locationCell(task)
            }
            .width(min: 80, ideal: 120)
            TableColumn("状态") { task in
                statusCell(task)
            }
            .width(min: 56, ideal: 72)
            TableColumn("进度") { task in
                progressCell(task)
            }
            .width(min: 44, ideal: 56)
            TableColumn("引擎") { task in
                Text(TaskCenterPresentation.engineDisplayName(for: task))
                    .lineLimit(1)
            }
            .width(min: 90, ideal: 110)
            TableColumn("时长") { task in
                Text(TaskCenterPresentation.durationText(for: task))
                    .lineLimit(1)
            }
            .width(min: 48, ideal: 60)
            TableColumn("输出") { task in
                outputCell(task)
            }
            .width(min: 80, ideal: 140)
            TableColumn("添加时间") { task in
                Text(task.createdAt, format: .dateTime.hour().minute())
                    .monospacedDigit()
            }
            .width(min: 56, ideal: 72)
        }
    }

    private func fileCell(_ task: BatchTask, foldLocation: Bool) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(task.sourceURL.lastPathComponent)
                .lineLimit(1)
                .truncationMode(.middle)
            if foldLocation {
                Text(TaskCenterPresentation.locationDisplay(for: task))
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
            }
        }
        .accessibilityLabel(fileAccessibilityLabel(for: task, showLocation: foldLocation))
        .help(foldLocation ? TaskCenterPresentation.locationTooltip(for: task) : "")
    }

    private func locationCell(_ task: BatchTask) -> some View {
        Text(TaskCenterPresentation.locationDisplay(for: task))
            .lineLimit(1)
            .accessibilityLabel(TaskCenterPresentation.locationTooltip(for: task))
            .help(TaskCenterPresentation.locationTooltip(for: task))
    }

    private func statusCell(_ task: BatchTask) -> some View {
        HStack(spacing: 4) {
            Image(systemName: TaskCenterPresentation.statusSymbolName(task.status))
                .foregroundStyle(statusColor(task.status))
            Text(TaskCenterPresentation.statusDisplayName(task.status))
                .lineLimit(1)
        }
        .accessibilityLabel(TaskCenterAccessibility.statusLabel(for: task.status))
    }

    private func statusColor(_ status: BatchTaskStatus) -> Color {
        switch status {
        case .completed: .green
        case .failed: .red
        case .extracting, .preparing, .exporting: .accentColor
        case .cancelled, .interrupted, .skipped, .waiting: .secondary
        }
    }

    private func progressCell(_ task: BatchTask) -> some View {
        Group {
            if let progressText = TaskCenterPresentation.progressText(for: task) {
                Text(progressText)
                    .monospacedDigit()
                    .accessibilityLabel(TaskCenterAccessibility.progressLabel(for: task) ?? progressText)
            } else {
                Text("—").foregroundStyle(.tertiary)
            }
        }
    }

    private func outputCell(_ task: BatchTask) -> some View {
        Text(task.outputURL?.lastPathComponent ?? "—")
            .lineLimit(1)
            .truncationMode(.middle)
    }

    private func fileAccessibilityLabel(for task: BatchTask, showLocation: Bool) -> String {
        var label = "文件 \(task.sourceURL.lastPathComponent)"
        if showLocation {
            label += " \(TaskCenterPresentation.locationTooltip(for: task))"
        }
        return label
    }
}
