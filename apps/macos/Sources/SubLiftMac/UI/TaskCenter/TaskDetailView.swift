import SwiftUI

/// 08511：Inspector 卡片（单任务详情；删除/上移/下移并入页眉）。
struct TaskDetailView: View {
    let task: BatchTask
    let inspectorModel: TaskCenterPresentation.TaskInspectorModel
    var onReplaceConfiguration: (ExtractionConfiguration) -> Void
    var onCancel: () -> Void
    var onRetry: () -> Void
    var onRemove: () -> Void
    var onMoveUp: () -> Void
    var onMoveDown: () -> Void

    /// 编辑中的引擎/质量（仅 waiting 可改；初始为任务当前配置）。
    @State private var editingEngine: OcrEngineName?
    @State private var editingQuality: SamplingQuality?

    private var canEdit: Bool { inspectorModel.canEditConfiguration }

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            cardHeader
            Divider()
            cardContent
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .frame(maxHeight: 240, alignment: .top)
        .background(Color(nsColor: .controlBackgroundColor))
        .onAppear {
            if editingEngine == nil {
                editingEngine = EngineCapability.normalizedSelection(
                    task.configuration.engine, developerMode: false
                )
            }
            if editingQuality == nil { editingQuality = task.configuration.quality }
        }
        .onChange(of: editingEngine) { _ in commitEditIfNeeded() }
        .onChange(of: editingQuality) { _ in commitEditIfNeeded() }
    }

    // MARK: - 页眉（文件名 + 状态 + 操作按钮）

    private var cardHeader: some View {
        HStack(alignment: .center, spacing: 8) {
            VStack(alignment: .leading, spacing: 2) {
                Text(inspectorModel.filename)
                    .font(.headline)
                    .lineLimit(1)
                    .truncationMode(.middle)
                Text(inspectorModel.statusName)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
            Spacer()
            HStack(spacing: 6) {
                if inspectorModel.canRemove {
                    Button { onRemove() } label: {
                        Label("删除", systemImage: "trash")
                    }
                    .help("删除该任务")
                }
                if inspectorModel.canReorder {
                    Button { onMoveUp() } label: {
                        Label("上移", systemImage: "arrow.up")
                    }
                    .help("上移该任务")
                    Button { onMoveDown() } label: {
                        Label("下移", systemImage: "arrow.down")
                    }
                    .help("下移该任务")
                }
                if inspectorModel.canCancel {
                    Button { onCancel() } label: {
                        Label("取消", systemImage: "xmark")
                    }
                    .help("取消该任务")
                }
                if inspectorModel.canRetry {
                    Button { onRetry() } label: {
                        Label("重试", systemImage: "arrow.counterclockwise")
                    }
                    .help("重试该任务")
                }
            }
            .labelStyle(.iconOnly)
            .buttonStyle(.borderless)
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
    }

    // MARK: - 卡片内容

    private var cardContent: some View {
        VStack(alignment: .leading, spacing: 6) {
            // 位置
            inspectorRow(label: "位置", value: inspectorModel.locationDisplay)
                .help(inspectorModel.locationFullPath)

            // 输出
            VStack(alignment: .leading, spacing: 2) {
                HStack(spacing: 4) {
                    Text("输出").font(.caption.weight(.semibold)).foregroundStyle(.secondary)
                        .frame(width: 48, alignment: .trailing)
                    Text("\(inspectorModel.outputFolderDisplay) / \(inspectorModel.outputFilename)")
                        .font(.caption)
                        .lineLimit(1)
                        .truncationMode(.middle)
                }
                if let warning = inspectorModel.outputExistsWarning {
                    Text(warning)
                        .font(.caption2)
                        .foregroundStyle(.orange)
                        .padding(.leading, 56)
                }
                if let error = inspectorModel.planningError {
                    Text(error)
                        .font(.caption2)
                        .foregroundStyle(.red)
                        .padding(.leading, 56)
                }
            }

            // 提取配置
            if canEdit {
                configurationRow
            } else {
                inspectorRow(label: "提取", value: "\(inspectorModel.engineDisplay) / \(inspectorModel.qualityDisplay)")
            }

            // 错误
            if let failure = inspectorModel.failureMessage {
                inspectorRow(label: "错误", value: failure)
                    .foregroundStyle(.red)
            }

            // Runtime
            if let runtime = inspectorModel.runtimeIdentity {
                inspectorRow(label: "Runtime", value: runtime)
            }
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
        .frame(maxHeight: .infinity, alignment: .top)
    }

    // MARK: - 辅助

    private func inspectorRow(label: String, value: String) -> some View {
        HStack(alignment: .top, spacing: 8) {
            Text(label)
                .font(.caption.weight(.semibold))
                .foregroundStyle(.secondary)
                .frame(width: 48, alignment: .trailing)
            Text(value)
                .font(.caption)
                .textSelection(.enabled)
                .lineLimit(2)
                .truncationMode(.middle)
        }
    }

    private var configurationRow: some View {
        HStack(spacing: 8) {
            Text("提取")
                .font(.caption.weight(.semibold))
                .foregroundStyle(.secondary)
                .frame(width: 48, alignment: .trailing)
            Picker("引擎", selection: $editingEngine) {
                ForEach(EngineCapability.visibleEngines(developerMode: false), id: \.self) { engine in
                    Text(engine.displayName).tag(Optional(engine))
                }
            }
            .pickerStyle(.menu)
            .labelsHidden()
            .frame(maxWidth: 140)

            Picker("质量", selection: $editingQuality) {
                ForEach(SamplingQuality.allCases, id: \.self) { quality in
                    Text(TaskCenterPresentation.qualityDisplayName(quality)).tag(Optional(quality))
                }
            }
            .pickerStyle(.menu)
            .labelsHidden()
            .frame(maxWidth: 100)
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
