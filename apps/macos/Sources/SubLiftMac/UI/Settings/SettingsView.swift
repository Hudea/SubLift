import SwiftUI

/// 10311：Settings 面板（General / Recognition / Advanced）。
///
/// - General：采样质量。
/// - Recognition：OCR 引擎（Mock 仅开发者模式）+ 真实 capability inline 状态。
/// - Advanced：Developer Mode、引擎 runtime 可见性；不提供假模型下载按钮。
struct SettingsView: View {
    @AppStorage("default_engine") var defaultEngine: OcrEngineName = .vision
    @AppStorage("sampling_quality") var samplingQuality: SamplingQuality = .fast
    @AppStorage("developer_mode") var developerMode: Bool = false

    @State private var selectedTab = SettingsPresentation.sections[0]
    /// 真实 capability 检测结果（onAppear 一次）。
    @State private var ffmpegAvailable = true
    @State private var paddleModelsAvailable = false

    var body: some View {
        TabView(selection: $selectedTab) {
            generalTab
                .tabItem { Label("General", systemImage: "gearshape") }
                .tag(SettingsPresentation.sections[0])
            recognitionTab
                .tabItem { Label("Recognition", systemImage: "text.viewfinder") }
                .tag(SettingsPresentation.sections[1])
            advancedTab
                .tabItem { Label("Advanced", systemImage: "wrench.and.screwdriver") }
                .tag(SettingsPresentation.sections[2])
        }
        .padding(24)
        .frame(width: 500, height: 340)
        .onAppear {
            normalizeDefaultEngine()
            ffmpegAvailable = FfmpegDetector.detect() != nil
            paddleModelsAvailable = Self.paddleModelsExist()
        }
        .onChange(of: developerMode) { _ in
            normalizeDefaultEngine()
        }
        .onChange(of: defaultEngine) { _ in
            normalizeDefaultEngine()
        }
    }

    // MARK: - General

    private var generalTab: some View {
        Form {
            Section {
                Picker("采样质量", selection: $samplingQuality) {
                    ForEach(SamplingQuality.allCases) { quality in
                        Text(quality.displayName).tag(quality)
                    }
                }
                .pickerStyle(.segmented)
                .accessibilityLabel("采样质量")

                Text(samplingQuality.helpText)
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } header: {
                Text("General")
            }
        }
        .formStyle(.grouped)
    }

    // MARK: - Recognition

    private var recognitionTab: some View {
        Form {
            Section {
                Picker("OCR 引擎", selection: normalizedDefaultEngineBinding) {
                    ForEach(EngineCapability.visibleEngines(developerMode: developerMode), id: \.self) { engine in
                        Text(engine.displayName).tag(engine)
                    }
                }
                .pickerStyle(.menu)
                .accessibilityLabel("OCR 引擎")
            } header: {
                Text("Recognition")
            }

            Section("引擎状态") {
                ForEach(EngineCapability.visibleEngines(developerMode: developerMode), id: \.self) { engine in
                    capabilityRow(for: engine)
                }
            }
        }
        .formStyle(.grouped)
    }

    private func capabilityRow(for engine: OcrEngineName) -> some View {
        let status = EngineCapability.status(
            for: engine,
            ffmpegAvailable: ffmpegAvailable,
            paddleModelsAvailable: paddleModelsAvailable,
            developerMode: developerMode
        )
        return VStack(alignment: .leading, spacing: 3) {
            HStack(spacing: 8) {
                Image(systemName: status.available ? "checkmark.circle.fill" : "exclamationmark.triangle.fill")
                    .foregroundStyle(status.available ? .green : .orange)
                    .accessibilityHidden(true)
                Text("\(engine.displayName)：\(status.summary)")
                    .font(.callout)
                Spacer(minLength: 0)
            }
            .accessibilityElement(children: .combine)
            if let detail = status.detail {
                Text(detail)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .accessibilityLabel(detail)
            }
        }
        .padding(.vertical, 2)
    }

    // MARK: - Advanced

    private var advancedTab: some View {
        Form {
            Section {
                Toggle("开发者模式（显示 Mock 测试引擎）", isOn: $developerMode)
                    .accessibilityLabel("开发者模式")
            } header: {
                Text("Advanced")
            }

            Section("运行时可见性") {
                ForEach(EngineCapability.visibleEngines(developerMode: developerMode), id: \.self) { engine in
                    let label = EngineCapability.runtimeLabel(
                        for: engine,
                        ffmpegAvailable: ffmpegAvailable,
                        paddleModelsAvailable: paddleModelsAvailable
                    )
                    HStack {
                        Text(engine.displayName)
                        Spacer()
                        Text(label)
                            .font(.system(.callout, design: .monospaced))
                            .foregroundStyle(.secondary)
                    }
                    .accessibilityElement(children: .combine)
                    .accessibilityLabel("\(engine.displayName)：\(label)")
                }
                Text("Native 是唯一运行时；能力不可用时失败关闭，回滚请使用上一已验收版本。")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            }
        }
        .formStyle(.grouped)
    }

    // MARK: - 真实检测

    /// Picker 的读取也经过归一化，因此即使 `onAppear` 尚未执行，菜单标签也不会空白。
    private var normalizedDefaultEngineBinding: Binding<OcrEngineName> {
        Binding(
            get: {
                EngineCapability.normalizedSelection(
                    defaultEngine,
                    developerMode: developerMode
                )
            },
            set: { selection in
                defaultEngine = EngineCapability.normalizedSelection(
                    selection,
                    developerMode: developerMode
                )
            }
        )
    }

    private func normalizeDefaultEngine() {
        let normalized = EngineCapability.normalizedSelection(
            defaultEngine,
            developerMode: developerMode
        )
        if normalized != defaultEngine {
            defaultEngine = normalized
        }
    }

    /// Paddle 模型目录（与 Python `DEFAULT_MODEL_DIR` 对齐）。
    private static func paddleModelsExist() -> Bool {
        let dir = FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent(".cache/sublift/rapidocr-models", isDirectory: true)
        guard let items = try? FileManager.default.contentsOfDirectory(atPath: dir.path) else { return false }
        return !items.isEmpty
    }
}
