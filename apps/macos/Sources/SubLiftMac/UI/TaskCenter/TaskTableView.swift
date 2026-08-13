import SwiftUI

/// 08308/08309：任务表格（原生 Table；多选；列优先级：文件 > 状态 > 进度 > 引擎 > 时长 > 输出 > 添加时间）。
/// 08511：按 visibleColumns 四档宽度条件渲染；位置不可见时折进文件单元格。
struct TaskTableView: View {
    let tasks: [BatchTask]
    @Binding var selection: Set<UUID>
    let visibleColumns: [TaskTableColumn]

    var body: some View {
        Table(tasks, selection: $selection) {
            TableColumn("文件") { task in
                let showLocation = !visibleColumns.contains(.location)
                VStack(alignment: .leading, spacing: 2) {
                    Text(task.sourceURL.lastPathComponent)
                        .lineLimit(1)
                        .truncationMode(.middle)
                    if showLocation {
                        Text(TaskCenterPresentation.locationDisplay(for: task))
                            .font(.caption)
                            .foregroundStyle(.secondary)
                            .lineLimit(1)
                    }
                }
                .accessibilityLabel(fileAccessibilityLabel(for: task, showLocation: showLocation))
                .help(showLocation ? TaskCenterPresentation.locationTooltip(for: task) : "")
            }
            .width(min: 140, ideal: 220)

            TableColumn("位置") { task in
                Text(TaskCenterPresentation.locationDisplay(for: task))
                    .lineLimit(1)
                    .accessibilityLabel(TaskCenterPresentation.locationTooltip(for: task))
                    .help(TaskCenterPresentation.locationTooltip(for: task))
            }
            .width(min: visibleColumns.contains(.location) ? 80 : 0,
                   ideal: visibleColumns.contains(.location) ? 120 : 0)

            TableColumn("状态") { task in
                Text(TaskCenterPresentation.statusDisplayName(task.status))
                    .lineLimit(1)
                    .accessibilityLabel(TaskCenterAccessibility.statusLabel(for: task.status))
            }
            .width(min: 56, ideal: 72)

            TableColumn("进度") { task in
                if let progressText = TaskCenterPresentation.progressText(for: task) {
                    Text(progressText)
                        .monospacedDigit()
                        .accessibilityLabel(TaskCenterAccessibility.progressLabel(for: task) ?? progressText)
                } else {
                    Text("—").foregroundStyle(.tertiary)
                }
            }
            .width(min: 44, ideal: 56)

            TableColumn("引擎") { task in
                Text(TaskCenterPresentation.engineDisplayName(for: task))
                    .lineLimit(1)
            }
            .width(min: visibleColumns.contains(.engine) ? 90 : 0,
                   ideal: visibleColumns.contains(.engine) ? 110 : 0)

            TableColumn("时长") { task in
                Text(TaskCenterPresentation.durationText(for: task))
                    .lineLimit(1)
            }
            .width(min: visibleColumns.contains(.duration) ? 48 : 0,
                   ideal: visibleColumns.contains(.duration) ? 60 : 0)

            TableColumn("输出") { task in
                Text(task.outputURL?.lastPathComponent ?? "—")
                    .lineLimit(1)
                    .truncationMode(.middle)
            }
            .width(min: visibleColumns.contains(.output) ? 80 : 0,
                   ideal: visibleColumns.contains(.output) ? 140 : 0)

            TableColumn("添加时间") { task in
                Text(task.createdAt, format: .dateTime.hour().minute())
                    .monospacedDigit()
            }
            .width(min: visibleColumns.contains(.added) ? 56 : 0,
                   ideal: visibleColumns.contains(.added) ? 72 : 0)
        }
    }

    private func fileAccessibilityLabel(for task: BatchTask, showLocation: Bool) -> String {
        var label = "文件 \(task.sourceURL.lastPathComponent)"
        if showLocation {
            label += " \(TaskCenterPresentation.locationTooltip(for: task))"
        }
        return label
    }
}
