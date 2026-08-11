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

/// 10208：Extraction 模式 Inspector 内容。
///
/// 展示质量、引擎、实际 runtime 与真实处理状态；engine/quality 选择从此处设置
/// （不再常驻主工作区）。不提供虚假 ETA/平均置信度或下载按钮。
struct ExtractionInspector: View {
    @ObservedObject var extractor: SubtitleExtractor
    let state: WorkspaceState

    @AppStorage("default_engine") private var defaultEngine: OcrEngineName = .vision
    @AppStorage("sampling_quality") private var samplingQuality: SamplingQuality = .fast

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            settingsSection
            Divider()
            infoSection
            Spacer(minLength: 0)
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 10)
        .accessibilityElement(children: .contain)
    }

    // MARK: - 设置（跨 Session 偏好）

    private var settingsSection: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("设置")
                .font(.caption.weight(.semibold))
                .foregroundStyle(.secondary)

            Picker("引擎", selection: $defaultEngine) {
                Text(OcrEngineName.vision.displayName).tag(OcrEngineName.vision)
                Text(OcrEngineName.paddle.displayName).tag(OcrEngineName.paddle)
                Text(OcrEngineName.mock.displayName).tag(OcrEngineName.mock)
            }
            .pickerStyle(.radioGroup)
            .disabled(extractor.isRunning)
            .accessibilityLabel("OCR 引擎")

            Picker("采样质量", selection: $samplingQuality) {
                ForEach(SamplingQuality.allCases) { quality in
                    Text(quality.displayName).tag(quality)
                }
            }
            .pickerStyle(.radioGroup)
            .disabled(extractor.isRunning)
            .help(samplingQuality.helpText)
            .accessibilityLabel("采样质量")
        }
    }

    // MARK: - 真实信息

    private var infoSection: some View {
        let rows = ExtractionInspectorPresentation.rows(
            engine: defaultEngine,
            quality: samplingQuality,
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
}
