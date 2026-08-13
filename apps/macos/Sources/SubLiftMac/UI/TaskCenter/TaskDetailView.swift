import SwiftUI

/// 08308/08309：选中任务详情（真实字段；08309：waiting 可显式改配置）。
struct TaskDetailView: View {
    let task: BatchTask
    var onReplaceConfiguration: (ExtractionConfiguration) -> Void
    var onCancel: () -> Void
    var onRetry: () -> Void

    /// 编辑中的引擎/质量（仅 waiting 可改；初始为任务当前配置）。
    @State private var editingEngine: OcrEngineName?
    @State private var editingQuality: SamplingQuality?

    private var canEdit: Bool {
        BatchTaskCommandAvailability.canReplaceConfiguration(task.status)
    }

    private var canCancel: Bool {
        BatchTaskCommandAvailability.canCancel(task.status)
    }

    private var canRetry: Bool {
        BatchTaskCommandAvailability.canRetry(task.status)
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
            actionButtons
        }
        .padding(10)
        .background(Color(nsColor: .controlBackgroundColor))
        .frame(maxHeight: 180, alignment: .top)
        .onAppear {
            if editingEngine == nil {
                // 归一化：持久化旧选择可能含 mock（关闭开发者模式后）——回落到可见引擎。
                editingEngine = EngineCapability.normalizedSelection(
                    task.configuration.engine, developerMode: false
                )
            }
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
                // Mock 仅开发者模式可见（app-wide 合同；普通 UI 不泄漏 Mock）。
                ForEach(EngineCapability.visibleEngines(developerMode: false), id: \.self) { engine in
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

    /// 单任务操作按钮（按 availability 启用；waiting/活动态可取消，failed/cancelled/interrupted 可重试）。
    private var actionButtons: some View {
        HStack(spacing: 10) {
            if canCancel {
                Button {
                    onCancel()
                } label: {
                    Label("取消任务", systemImage: "xmark")
                }
                .help("取消该任务")
                .accessibilityLabel("取消任务")
            }
            if canRetry {
                Button {
                    onRetry()
                } label: {
                    Label("重试", systemImage: "arrow.counterclockwise")
                }
                .help("重新排队该任务")
                .accessibilityLabel("重试任务")
            }
            Spacer()
        }
    }
}
