import SwiftUI

/// 08308/08309：任务表格（原生 Table；多选；列优先级：文件 > 状态 > 进度 > 引擎 > 时长 > 输出 > 添加时间）。
struct TaskTableView: View {
    let tasks: [BatchTask]
    @Binding var selection: Set<UUID>

    var body: some View {
        Table(tasks, selection: $selection) {
            TableColumn("文件") { task in
                Text(task.sourceURL.lastPathComponent)
                    .lineLimit(1)
                    .truncationMode(.middle)
                    .accessibilityLabel("文件 \(task.sourceURL.lastPathComponent)")
            }
            .width(min: 140, ideal: 220)

            TableColumn("状态") { task in
                Text(TaskCenterPresentation.statusDisplayName(task.status))
                    .lineLimit(1)
                    // 08309：AX label 走 TaskCenterAccessibility 单一真源。
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
            .width(min: 90, ideal: 110)

            TableColumn("时长") { task in
                Text(TaskCenterPresentation.durationText(for: task))
                    .lineLimit(1)
            }
            .width(min: 48, ideal: 60)

            TableColumn("输出") { task in
                Text(task.outputURL?.lastPathComponent ?? "—")
                    .lineLimit(1)
                    .truncationMode(.middle)
            }
            .width(min: 80, ideal: 140)

            TableColumn("添加时间") { task in
                Text(task.createdAt, format: .dateTime.hour().minute())
                    .monospacedDigit()
            }
            .width(min: 56, ideal: 72)
        }
    }
}
