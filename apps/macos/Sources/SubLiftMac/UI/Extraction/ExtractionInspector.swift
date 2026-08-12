import SwiftUI

/// UI 展示名映射（不污染 Core 枚举）。
extension OcrEngineName {
    var displayName: String {
        switch self {
        case .vision: "Apple Vision"
        case .paddle: "PaddleOCR"
        case .mock: "Mock 引擎"
        }
    }
}

/// 10208：Extraction Inspector 展示纯逻辑（真实配置与 runtime）。
enum ExtractionInspectorPresentation {

    struct Row: Equatable {
        let label: String
        let value: String
        let icon: String
    }

    static func rows(
        engine: OcrEngineName,
        quality: SamplingQuality,
        runtimeIdentity: String?,
        state: WorkspaceState
    ) -> [Row] {
        [
            Row(label: "引擎", value: engine.displayName, icon: "cpu"),
            Row(label: "质量", value: quality.displayName, icon: "slider.horizontal.3"),
            Row(label: "运行时", value: runtimeIdentity ?? "—", icon: "terminal"),
            Row(label: "状态", value: stateText(state), icon: stateIcon(state)),
        ]
    }

    private static func stateText(_ state: WorkspaceState) -> String {
        switch state {
        case .empty, .loading, .ready: "就绪"
        case .regionEditing: "区域编辑中"
        case .starting: "正在启动"
        case .processing: "提取中"
        case .finalizing: "整理中"
        case .review: "完成"
        case .failed: "失败"
        case .cancelled: "已取消"
        }
    }

    private static func stateIcon(_ state: WorkspaceState) -> String {
        switch state {
        case .processing, .starting, .finalizing: "waveform.path.ecg"
        case .review: "checkmark.circle"
        case .failed: "exclamationmark.triangle"
        case .cancelled: "xmark.circle"
        default: "circle"
        }
    }
}

/// 10208：Extraction 模式 Inspector 内容（10415 去重后）。
///
/// 只读展示实际运行配置（active 优先、final 其次、偏好最后）、runtime 与真实状态；
/// 不再提供可编辑引擎/采样控件（Settings 与快速设置栏是仅有的偏好编辑入口）。
/// 不提供虚假 ETA/平均置信度或下载按钮。
struct ExtractionInspector: View {
    @ObservedObject var extractor: SubtitleExtractor
    let state: WorkspaceState
    let activeConfiguration: ExtractionConfiguration?
    let finalConfiguration: ExtractionConfiguration?

    /// 偏好仅作展示 fallback（跨 Session 编辑入口在 Settings 与快速设置栏）。
    @AppStorage("default_engine") private var defaultEngine: OcrEngineName = .vision
    @AppStorage("sampling_quality") private var samplingQuality: SamplingQuality = .fast
    @AppStorage("developer_mode") private var developerMode: Bool = false

    /// 偏好回退同样归一化：陈旧 Mock 偏好 + 关闭开发者模式时展示 Vision，
    /// 与快速设置栏/请求路径保持一致（10414 展示归一化不回归）。
    private var preferences: ExtractionConfiguration {
        ExtractionConfiguration(
            engine: EngineCapability.normalizedSelection(defaultEngine, developerMode: developerMode),
            quality: samplingQuality
        )
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            infoSection
            Divider()
            noteSection
            Spacer(minLength: 0)
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 10)
        .accessibilityElement(children: .contain)
    }

    // MARK: - 真实信息（运行配置快照 + runtime + 状态）

    private var infoSection: some View {
        let configuration = ExtractionInspectorConfiguration.displayConfiguration(
            active: activeConfiguration,
            final: finalConfiguration,
            preferences: preferences
        )
        let rows = ExtractionInspectorPresentation.rows(
            engine: configuration.engine,
            quality: configuration.quality,
            runtimeIdentity: extractor.runtimeIdentity,
            state: state
        )
        return VStack(alignment: .leading, spacing: 0) {
            ForEach(Array(rows.enumerated()), id: \.offset) { _, row in
                HStack(spacing: 8) {
                    Image(systemName: row.icon)
                        .font(.system(size: 12))
                        .foregroundStyle(.secondary)
                        .frame(width: 16)
                    Text(row.label)
                        .font(.callout)
                        .foregroundStyle(.secondary)
                    Spacer(minLength: 4)
                    Text(row.value)
                        .font(.system(.callout, design: .monospaced))
                        .lineLimit(1)
                        .multilineTextAlignment(.trailing)
                }
                .padding(.vertical, 5)
                .accessibilityElement(children: .combine)
                .accessibilityLabel("\(row.label)：\(row.value)")
            }
        }
    }

    // MARK: - 说明（编辑入口提示）

    private var noteSection: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text("引擎与采样可在设置或快速提取设置栏中更改。")
                .font(.caption)
                .foregroundStyle(.secondary)
                .accessibilityLabel("引擎与采样可在设置或快速提取设置栏中更改")
            if activeConfiguration != nil {
                Text("当前任务使用启动时的配置。")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .accessibilityLabel("当前任务使用启动时的配置")
            }
        }
    }
}
