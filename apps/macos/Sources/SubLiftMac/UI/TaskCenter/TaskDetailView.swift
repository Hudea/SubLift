import SwiftUI

/// 08308：选中任务详情（真实字段；源/配置/runtime/错误/输出/Task ID）。
struct TaskDetailView: View {
    let task: BatchTask

    var body: some View {
        let rows = TaskCenterPresentation.detailRows(for: task)
        VStack(alignment: .leading, spacing: 4) {
            ForEach(rows, id: \.label) { row in
                HStack(alignment: .top, spacing: 8) {
                    Text(row.label)
                        .font(.caption.weight(.semibold))
                        .foregroundStyle(.secondary)
                        .frame(width: 72, alignment: .trailing)
                    Text(row.value)
                        .font(.caption)
                        .textSelection(.enabled)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
                .accessibilityElement(children: .combine)
                .accessibilityLabel("\(row.label)：\(row.value)")
            }
        }
        .padding(10)
        .background(Color(nsColor: .controlBackgroundColor))
        .frame(maxHeight: 120, alignment: .top)
    }
}
