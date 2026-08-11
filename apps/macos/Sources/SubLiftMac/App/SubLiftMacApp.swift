import SwiftUI
import UniformTypeIdentifiers

@main
struct SubLiftMacApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var appDelegate

    var body: some Scene {
        WindowGroup {
            ContentView()
        }
        .defaultSize(width: 1280, height: 800)
        Settings {
            SettingsView()
        }
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.regular)
        NSApp.activate(ignoringOtherApps: true)
    }
}

// MARK: - ContentView

struct ContentView: View {
    @StateObject private var workspace = WorkspaceModel()

    private var playerModel: PlayerModel { workspace.playerModel }
    private var extractor: SubtitleExtractor { workspace.extractor }
    private var editor: SubtitleEditor { workspace.editor }
    private var metadataLoader: VideoMetadataLoader { workspace.metadataLoader }
    private var regionModel: RegionSelectionModel { workspace.regionModel }
    @AppStorage("default_engine") private var defaultEngine: OcrEngineName = .vision
    @AppStorage("sampling_quality") private var samplingQuality: SamplingQuality = .fast
    @State private var showFfmpegMissingAlert = false
    @State private var exportError: String?
    @State private var showExportSuccess = false
    @State private var isDropTargeted = EvidenceShot.isDropTargetFixture
    @State private var dropErrorMessage: String?

    var body: some View {
        DropTargetView(isTargeted: $isDropTargeted, onDrop: importVideo) {
            Group {
                if let url = workspace.currentVideoURL {
                    HSplitView {
                        // 左侧：预览 + 控制 + 元数据 + 提取
                        leftPane(url: url)
                        .frame(minWidth: 400)

                        // 右侧：字幕列表 + 编辑
                        SubtitleList(
                            editor: editor,
                            onSeek: { ms in
                                if playerModel.loadFailed {
                                    playerModel.fallbackSeek(toMs: ms)
                                } else {
                                    playerModel.seek(toMs: ms)
                                }
                            },
                            onExport: exportSRT,
                            accessMode: workspace.transcriptAccessMode,
                            commands: workspace.commandAvailability
                        )
                            .frame(minWidth: 360)
                    }
                } else {
                    WelcomeView(
                        onOpen: openFile,
                        isDropTargeted: isDropTargeted || EvidenceShot.isDropTargetFixture
                    )
                }
            }
            .frame(minWidth: 800, minHeight: 480)
            .overlay(alignment: .top) {
                if isDropTargeted, workspace.currentVideoURL != nil {
                    Text("松开以打开新视频")
                        .font(.callout.weight(.semibold))
                        .foregroundStyle(Color.accentColor)
                        .padding(.horizontal, 14)
                        .padding(.vertical, 8)
                        .background(.regularMaterial, in: Capsule())
                        .padding(.top, 10)
                        .transition(.opacity)
                }
            }
            .overlay(alignment: .bottom) {
                if let dropErrorMessage {
                    dropErrorBanner(dropErrorMessage)
                }
            }
            .animation(.easeOut(duration: 0.15), value: isDropTargeted)
            .alert("需要 ffmpeg", isPresented: $showFfmpegMissingAlert) {
                Button("好", role: .cancel) { }
            } message: {
                VStack(alignment: .leading, spacing: 8) {
                    Text("此 MKV 视频需要 ffmpeg 才能预览和提取。请安装 ffmpeg 后重新打开视频。")
                    Text("brew install ffmpeg")
                        .font(.system(.body, design: .monospaced))
                        .foregroundStyle(.secondary)
                }
            }
            .alert("导出失败", isPresented: Binding(
                get: { exportError != nil },
                set: { if !$0 { exportError = nil } }
            )) {
                Button("确定", role: .cancel) { }
            } message: {
                if let exportError {
                    Text(exportError)
                }
            }
            .alert("导出成功", isPresented: $showExportSuccess) {
                Button("确定", role: .cancel) { }
            } message: {
                Text("SRT 字幕已保存。")
            }
            .onAppear {
                #if DEBUG
                EvidenceShot.scheduleIfRequested()
                #endif
            }
        }
    }

    // MARK: - 预览（叠加 Vision 候选框）

    private func leftPane(url: URL) -> some View {
        GeometryReader { geometry in
            VStack(spacing: 0) {
                previewSection(height: PreviewLayout.height(
                    containerSize: geometry.size,
                    videoSize: CGSize(
                        width: metadataLoader.metadata?.width ?? 0,
                        height: metadataLoader.metadata?.height ?? 0
                    )
                ))
                Divider()
                VideoControlsView(model: playerModel)
                Divider()
                metadataBar
                Divider()
                RegionCandidateList(model: regionModel) {
                    redetectRegionAtPlayhead()
                }
                Divider()
                extractionBar(url: url)
                Spacer(minLength: 0)
            }
        }
    }

    @ViewBuilder
    private func previewSection(height: CGFloat) -> some View {
        let width = metadataLoader.metadata?.width ?? 0
        let videoHeight = metadataLoader.metadata?.height ?? 0

        PreviewRegionContainer(
            regionModel: regionModel,
            videoWidth: width,
            videoHeight: videoHeight
        ) {
            if playerModel.loadFailed {
                unsupportedPreview
            } else {
                VideoPreview(model: playerModel)
            }
        }
        .frame(maxWidth: .infinity)
        .frame(height: height)
        .clipped()
    }

    // MARK: - 元数据栏

    private var metadataBar: some View {
        HStack(spacing: 16) {
            if let meta = metadataLoader.metadata {
                Label {
                    Text(meta.fileName)
                        .lineLimit(1)
                        .truncationMode(.middle)
                } icon: {
                    Image(systemName: "film")
                }

                Divider()
                    .frame(height: 14)

                Label(VideoMetadata.formatResolution(width: meta.width, height: meta.height), systemImage: "aspectratio")

                Divider()
                    .frame(height: 14)

                Label(TimeFormatter.formatMs(meta.durationMs), systemImage: "clock")

                Divider()
                    .frame(height: 14)

                Label(meta.codec, systemImage: "cpu")

                Divider()
                    .frame(height: 14)

                Label(VideoMetadata.formatFileSize(meta.fileSize), systemImage: "doc")

                Spacer()
            } else {
                ProgressView()
                    .scaleEffect(0.7)
                Text("解析元数据...")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Spacer()
            }
        }
        .font(.system(.caption, design: .monospaced))
        .padding(.horizontal, 12)
        .padding(.vertical, 6)
    }

    // MARK: - 提取栏

    private func extractionBar(url: URL) -> some View {
        VStack(spacing: 8) {
            HStack(spacing: 12) {
                Button {
                    workspace.startExtraction(
                        engine: defaultEngine,
                        quality: samplingQuality
                    )
                } label: {
                    Label("提取字幕", systemImage: "text.viewfinder")
                }
                .buttonStyle(.borderedProminent)
                .disabled(!workspace.commandAvailability.canExtract)

                if workspace.commandAvailability.canStop {
                    Button("取消") { workspace.cancelExtraction() }
                        .buttonStyle(.bordered)
                }

                Spacer(minLength: 0)

                Picker("", selection: $defaultEngine) {
                    Text("Apple Vision").tag(OcrEngineName.vision)
                    Text("PaddleOCR").tag(OcrEngineName.paddle)
                    Text("Mock 引擎").tag(OcrEngineName.mock)
                }
                .labelsHidden()
                .frame(width: 130)
                .disabled(extractor.isRunning)
            }

            HStack(spacing: 12) {

                Picker("采样", selection: $samplingQuality) {
                    ForEach(SamplingQuality.allCases) { quality in
                        Text(quality.displayName).tag(quality)
                    }
                }
                .pickerStyle(.segmented)
                .frame(width: 180)
                .help(samplingQuality.helpText)
                .disabled(extractor.isRunning)
                .accessibilityLabel("采样密度")

                statusLabel
                    .lineLimit(1)
                    .truncationMode(.tail)
                    .frame(minWidth: 100, maxWidth: .infinity, alignment: .trailing)
            }

            if let runtimeIdentity = extractor.runtimeIdentity {
                Text(runtimeIdentity)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .frame(maxWidth: .infinity, alignment: .trailing)
                    .accessibilityLabel("OCR 运行时：\(runtimeIdentity)")
            }

            switch extractor.status {
            case .processing(let progress, _, _):
                ProgressView(value: progress)
                    .progressViewStyle(.linear)
            case .finalizing:
                ProgressView()
                    .progressViewStyle(.linear)
            default:
                EmptyView()
            }
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
    }

    @ViewBuilder
    private var statusLabel: some View {
        switch extractor.status {
        case .idle:
            Text("就绪").foregroundStyle(.secondary)
        case .startingServer:
            Text("正在启动服务...").foregroundStyle(.orange)
        case .processing(let progress, let frameCount, let totalFrames):
            HStack(spacing: 6) {
                Text("提取与识别中 \(frameCount)/\(totalFrames) (\(Int(progress * 100))%)")
                    .foregroundStyle(.blue)
                if let rateText = ProcessingRate.displayText(for: extractor.processingRate) {
                    Text(rateText)
                        .foregroundStyle(.secondary)
                }
            }
            .font(.system(.body, design: .monospaced))
        case .finalizing:
            Text("正在整理字幕...").foregroundStyle(.orange)
        case .done(let count):
            Text("完成，识别 \(count) 条")
                .foregroundStyle(.green)
        case .error(let msg):
            Text(msg).foregroundStyle(.red)
        }
    }

    // MARK: - 不支持的预览（mkv 等 AVPlayer 无法播放的格式）

    private var unsupportedPreview: some View {
        ZStack {
            Color.black

            if let image = playerModel.fallbackPreview {
                Image(nsImage: image)
                    .resizable()
                    .aspectRatio(contentMode: .fit)
            } else {
                ProgressView("正在提取预览帧...")
                    .tint(.white)
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .overlay(alignment: .bottom) {
            if playerModel.fallbackPreview != nil {
                Text("静态预览（拖动进度条查看，mkv 不支持播放）")
                    .font(.caption)
                    .foregroundStyle(.white.opacity(0.7))
                    .padding(6)
                    .background(Color.black.opacity(0.5))
                    .padding(.bottom, 4)
            }
        }
    }

    // MARK: - 导入意图（Open Panel 与 drop 共用）

    private func openFile() {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = VideoImportPolicy.openPanelContentTypes
        panel.allowsMultipleSelection = false
        panel.canChooseDirectories = false
        panel.message = "选择 MP4、MOV 或 MKV 视频"
        if panel.runModal() == .OK {
            if let url = panel.url {
                _ = importVideo(url)
            }
        }
    }

    /// 统一导入 intent：先校验（fail-closed），再交给 WorkspaceModel 打开。
    /// - 非法格式：返回 false 并给出 inline 图标+文字反馈（不改变 Session）。
    /// - MKV 缺 ffmpeg：返回 false 并弹出 actionable alert（不改变 Session）。
    /// - 状态不允许打开（如 processing）：返回 false 并给出明确反馈，避免"松开以打开"承诺落空。
    @discardableResult
    private func importVideo(_ url: URL) -> Bool {
        switch workspace.validateImport(url) {
        case .valid:
            dropErrorMessage = nil
            if workspace.openVideo(url: url) {
                return true
            }
            dropErrorMessage = "当前无法打开新视频。"
            return false
        case .unsupportedFormat(let ext):
            dropErrorMessage = ext.isEmpty
                ? "无法打开：仅支持 MP4、MOV、MKV 视频。"
                : "无法打开 .\(ext)：仅支持 MP4、MOV、MKV 视频。"
            return false
        case .mkvRequiresFfmpeg:
            showFfmpegMissingAlert = true
            return false
        }
    }

    /// 非法拖入反馈：图标 + 文字（非纯颜色），4 秒后自动消失。
    private func dropErrorBanner(_ message: String) -> some View {
        Label(message, systemImage: "exclamationmark.triangle.fill")
            .font(.callout)
            .foregroundStyle(.red)
            .padding(.horizontal, 14)
            .padding(.vertical, 10)
            .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 8))
            .padding(.bottom, 16)
            .accessibilityElement(children: .combine)
            .accessibilityAddTraits(.isStaticText)
            .task(id: message) {
                try? await Task.sleep(nanoseconds: 4_000_000_000)
                if dropErrorMessage == message {
                    dropErrorMessage = nil
                }
            }
    }

    /// 在当前播放位置重跑 Vision 选区（用户可先 seek 到有字幕的画面）。
    private func redetectRegionAtPlayhead() {
        guard let meta = metadataLoader.metadata, let url = workspace.currentVideoURL else { return }
        let seconds = Double(playerModel.currentMs) / 1000.0
        Task {
            await regionModel.detectAt(
                url: url,
                videoWidth: meta.width,
                videoHeight: meta.height,
                sampleSeconds: seconds
            )
        }
    }

    private func exportSRT() {
        guard workspace.commandAvailability.canExport else { return }
        let entries = editor.exportEntries()
        guard !entries.isEmpty else { return }

        let panel = NSSavePanel()
        panel.allowedContentTypes = [UTType(filenameExtension: "srt")!]
        panel.nameFieldStringValue = defaultSRTFilename()
        panel.canCreateDirectories = true

        guard panel.runModal() == .OK, let url = panel.url else { return }

        do {
            let srt = try SrtFormatter.format(entries: entries)
            try srt.write(to: url, atomically: true, encoding: .utf8)
            showExportSuccess = true
        } catch let error as SrtFormatError {
            exportError = error.localizedDescription
        } catch {
            exportError = "保存文件失败: \(error.localizedDescription)"
        }
    }

    private func defaultSRTFilename() -> String {
        guard let videoURL = workspace.currentVideoURL else { return "subtitle.srt" }
        return videoURL.deletingPathExtension().appendingPathExtension("srt").lastPathComponent
    }
}
