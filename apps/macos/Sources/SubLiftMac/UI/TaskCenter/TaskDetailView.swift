import SwiftUI

/// 08308/08309：选中任务详情（真实字段；08309：waiting 可显式改配置）。
struct TaskDetailView: View {
    let task: BatchTask
    var onReplaceConfiguration: (ExtractionConfiguration) -> Void

    /// 编辑中的引擎/质量（仅 waiting 可改；初始为任务当前配置）。
    @State private var editingEngine: OcrEngineName?
    @State private var editingQuality: SamplingQuality?

    private var canEdit: Bool {
        BatchTaskCommandAvailability.canReplaceConfiguration(task.status)
    }

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
            if canEdit {
                configurationEditor
            }
        }
        .padding(10)
        .background(Color(nsColor: .controlBackgroundColor))
        .frame(maxHeight: 160, alignment: .top)
        .onAppear {
            if editingEngine == nil { editingEngine = task.configuration.engine }
            if editingQuality == nil { editingQuality = task.configuration.quality }
        }
        .onChange(of: editingEngine) { newValue in
            commitEditIfNeeded()
        }
        .onChange(of: editingQuality) { newValue in
            commitEditIfNeeded()
        }
    }

    /// 08309：显式改配置（仅 waiting；活动/终态只读）。
    private var configurationEditor: some View {
        HStack(spacing: 10) {
            Text("引擎")
                .font(.caption.weight(.semibold))
                .foregroundStyle(.secondary)
            Picker("引擎", selection: $editingEngine) {
                ForEach(OcrEngineName.allCases, id: \.self) { engine in
                    Text(engine.displayName).tag(Optional(engine))
                }
            }
            .pickerStyle(.menu)
            .frame(maxWidth: 180)
            .accessibilityLabel("引擎")

            Text("质量")
                .font(.caption.weight(.semibold))
                .foregroundStyle(.secondary)
            Picker("质量", selection: $editingQuality) {
                ForEach(SamplingQuality.allCases, id: \.self) { quality in
                    Text(TaskCenterPresentation.qualityDisplayName(quality)).tag(Optional(quality))
                }
            }
            .pickerStyle(.menu)
            .frame(maxWidth: 120)
            .accessibilityLabel("质量")
            Spacer()
        }
    }

    private func commitEditIfNeeded() {
        guard let engine = editingEngine, let quality = editingQuality else { return }
        let current = task.configuration
        if engine != current.engine || quality != current.quality {
            onReplaceConfiguration(ExtractionConfiguration(engine: engine, quality: quality))
        }
    }
}
