import SwiftUI

// MARK: - 展示纯逻辑（10415）

/// 快速设置栏展示策略：配置生效状态、重新提取可见性与运行锁定。
enum QuickExtractionSettingsPresentation {

    /// 待生效提示：已有最终结果配置且偏好与其不同；运行中/区域编辑中不显示
    /// （此时任务已按启动时快照运行，提示会误导）。
    static func showsPendingNotice(
        preferences: ExtractionConfiguration,
        finalConfiguration: ExtractionConfiguration?,
        state: WorkspaceState
    ) -> Bool {
        guard !isLocked(state) else { return false }
        guard let finalConfiguration else { return false }
        return finalConfiguration != preferences
    }

    /// 设置栏内"重新提取"按钮：仅已有最终结果且配置变化时显示
    /// （Toolbar 的提取入口在 review 态始终可用，设置栏不重复常驻按钮）。
    static func showsReextractButton(
        hasFinalEntries: Bool,
        preferences: ExtractionConfiguration,
        finalConfiguration: ExtractionConfiguration?,
        state: WorkspaceState
    ) -> Bool {
        guard hasFinalEntries else { return false }
        return showsPendingNotice(preferences: preferences, finalConfiguration: finalConfiguration, state: state)
    }

    /// 运行中（starting/processing/finalizing）或区域编辑中控件锁定。
    /// regionEditing 期间提取不可达（startExtraction guard 拒绝），锁定避免静默 no-op。
    static func isLocked(_ state: WorkspaceState) -> Bool {
        switch state {
        case .starting, .processing, .finalizing, .regionEditing: true
        default: false
        }
    }

    /// 待生效提示文案（图标 + 文字，不依赖颜色表达状态）。
    static let pendingNoticeText = "配置已更改，重新提取后生效"
}

/// Inspector 配置展示优先级：active（运行中）→ final（最近成功）→ 偏好。
enum ExtractionInspectorConfiguration {

    static func displayConfiguration(
        active: ExtractionConfiguration?,
        final: ExtractionConfiguration?,
        preferences: ExtractionConfiguration
    ) -> ExtractionConfiguration {
        if let active { return active }
        if let final { return final }
        return preferences
    }
}

/// 提取请求策略：已有最终结果时需替换确认；Mock 归一化与配置冻结。
enum ExtractionRequestPolicy {

    static func requiresReplacementConfirmation(hasFinalEntries: Bool) -> Bool {
        hasFinalEntries
    }

    static let replacementConfirmationMessage = "重新提取将替换当前字幕结果。"
    static let replacementConfirmationTitle = "替换现有字幕？"

    /// 提取请求入口的配置冻结：先归一化引擎（非开发模式 Mock 回落 Vision），
    /// 再冻结为不可变快照。requestExtraction 与测试共用同一策略。
    static func normalizedConfiguration(
        engine: OcrEngineName,
        quality: SamplingQuality,
        developerMode: Bool
    ) -> ExtractionConfiguration {
        ExtractionConfiguration(
            engine: EngineCapability.normalizedSelection(engine, developerMode: developerMode),
            quality: quality
        )
    }
}

// MARK: - QuickExtractionSettingsBar

/// 10415：紧凑快速提取设置栏（ExtractionProgressView 下方）。
///
/// 引擎与采样写入现有 @AppStorage 偏好（与 Settings 共用同一真源）；
/// 运行中禁用但值可读；待生效提示与"重新提取"按配置快照状态显示。
struct QuickExtractionSettingsBar: View {
    let state: WorkspaceState
    let hasFinalEntries: Bool
    let finalConfiguration: ExtractionConfiguration?
    let onRequestExtraction: () -> Void

    @AppStorage("default_engine") private var defaultEngine: OcrEngineName = .vision
    @AppStorage("sampling_quality") private var samplingQuality: SamplingQuality = .fast
    @AppStorage("developer_mode") private var developerMode: Bool = false

    private var preferences: ExtractionConfiguration {
        ExtractionConfiguration(engine: normalizedEngine, quality: samplingQuality)
    }

    private var normalizedEngine: OcrEngineName {
        EngineCapability.normalizedSelection(defaultEngine, developerMode: developerMode)
    }

    private var engineBinding: Binding<OcrEngineName> {
        Binding(
            get: { normalizedEngine },
            set: { selection in
                defaultEngine = EngineCapability.normalizedSelection(selection, developerMode: developerMode)
            }
        )
    }

    private var locked: Bool {
        QuickExtractionSettingsPresentation.isLocked(state)
    }

    var body: some View {
        VStack(spacing: 4) {
            headerRow
            controlsRow
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 6)
        .frame(minHeight: 52, maxHeight: 64, alignment: .top)
        .accessibilityElement(children: .contain)
    }

    // MARK: - 标题行（标题 / 待生效提示 / 重新提取）

    private var headerRow: some View {
        HStack(spacing: 10) {
            Text("提取设置")
                .font(.caption.weight(.semibold))
                .foregroundStyle(.secondary)
                .accessibilityLabel("提取设置")

            if QuickExtractionSettingsPresentation.showsPendingNotice(
                preferences: preferences,
                finalConfiguration: finalConfiguration,
                state: state
            ) {
                Label(QuickExtractionSettingsPresentation.pendingNoticeText, systemImage: "info.circle")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                    .accessibilityElement(children: .combine)
            }

            Spacer(minLength: 8)

            if QuickExtractionSettingsPresentation.showsReextractButton(
                hasFinalEntries: hasFinalEntries,
                preferences: preferences,
                finalConfiguration: finalConfiguration,
                state: state
            ) {
                // 运行中 hasFinalEntries 必为 false，按钮在 locked 时不显示；
                // disabled 保留为防御性（区域编辑等状态）。
                Button("重新提取", action: onRequestExtraction)
                    .disabled(locked)
                    .help("按当前设置重新提取字幕")
                    .accessibilityLabel("重新提取")
                    .accessibilityHint("将替换当前字幕结果")
            }
        }
    }

    // MARK: - 控件行（引擎 Popup + 采样分段）

    private var controlsRow: some View {
        HStack(spacing: 12) {
            Picker("OCR 引擎", selection: engineBinding) {
                ForEach(EngineCapability.visibleEngines(developerMode: developerMode), id: \.self) { engine in
                    Text(engine.displayName).tag(engine)
                }
            }
            .pickerStyle(.menu)
            .labelsHidden()
            .disabled(locked)
            .help("OCR 引擎")
            .accessibilityLabel("OCR 引擎")
            .accessibilityValue(normalizedEngine.displayName)

            ViewThatFits {
                Picker("采样质量", selection: $samplingQuality) {
                    ForEach(SamplingQuality.allCases) { quality in
                        Text(quality.displayName).tag(quality)
                    }
                }
                .pickerStyle(.segmented)
                .labelsHidden()
                .disabled(locked)
                .help(samplingQuality.helpText)
                .accessibilityLabel("采样质量")
                .accessibilityValue(samplingQuality.displayName)

                Picker("采样质量", selection: $samplingQuality) {
                    ForEach(SamplingQuality.allCases) { quality in
                        Text(quality.displayName).tag(quality)
                    }
                }
                .pickerStyle(.menu)
                .labelsHidden()
                .disabled(locked)
                .help(samplingQuality.helpText)
                .accessibilityLabel("采样质量")
                .accessibilityValue(samplingQuality.displayName)
            }

            Spacer(minLength: 0)
        }
    }
}
