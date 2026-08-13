import AppKit
import SwiftUI
import UniformTypeIdentifiers

/// 10104：Native Workspace 根视图。
///
/// 组合 Welcome / Video / Transcript Split、系统 Toolbar 与统一拖拽导入；
/// 命令 availability 与 action 全部来自 WorkspaceModel（View 只投影状态、发送 intent）。
/// 窗口尺寸：Welcome 720×560（最小 640×520）；打开视频后升到工作台 1280×800（最小 960×600）。
struct WorkspaceRootView: View {
    @ObservedObject var workspace: WorkspaceModel
    @Environment(\.openWindow) private var openWindow

    @AppStorage("default_engine") private var defaultEngine: OcrEngineName = .vision
    @AppStorage("sampling_quality") private var samplingQuality: SamplingQuality = .fast
    @AppStorage("developer_mode") private var developerMode = false

    @State private var isDropTargeted: Bool = {
        #if DEBUG
        EvidenceShot.isDropTargetFixture
        #else
        false
        #endif
    }()
    @State private var dropErrorMessage: String?
    @State private var showFfmpegMissingAlert = false
    @State private var exportError: String?
    @State private var showExportSuccess = false
    /// 替换现有字幕确认（有最终结果时统一提取入口弹出；Cancel 为安全默认）。
    @State private var showReplacementConfirmation = false
    @State private var pendingExtractionConfiguration: ExtractionConfiguration?

    var body: some View {
        DropTargetView(isTargeted: $isDropTargeted, onDrop: importVideo) {
            Group {
                if let url = workspace.currentVideoURL {
                    workspaceSplit(url: url)
                } else {
                    WelcomeView(
                        onOpen: openFile,
                        isDropTargeted: isDropTargeted || {
                            #if DEBUG
                            EvidenceShot.isDropTargetFixture
                            #else
                            false
                            #endif
                        }()
                    )
                }
            }
            .frame(
                minWidth: workspace.currentVideoURL == nil
                    ? WorkspaceLayout.welcomeWindowMinSize.width
                    : WorkspaceLayout.workbenchWindowMinSize.width,
                minHeight: workspace.currentVideoURL == nil
                    ? WorkspaceLayout.welcomeWindowMinSize.height
                    : WorkspaceLayout.workbenchWindowMinSize.height
            )
            .background {
                WorkspaceEscapeKeyMonitorView(workspace: workspace)
                    .frame(width: 0, height: 0)
            }
            .toolbar { toolbarContent }
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
            .alert(ExtractionRequestPolicy.replacementConfirmationTitle, isPresented: $showReplacementConfirmation) {
                Button("重新提取", role: .destructive) {
                    if let config = pendingExtractionConfiguration {
                        workspace.startExtraction(engine: config.engine, quality: config.quality)
                    }
                    pendingExtractionConfiguration = nil
                }
                Button("取消", role: .cancel) {
                    // 取消不得清空或修改现有字幕与配置。
                    pendingExtractionConfiguration = nil
                }
            } message: {
                Text(ExtractionRequestPolicy.replacementConfirmationMessage)
            }
            .onReceive(NotificationCenter.default.publisher(for: .subliftRequestOpenVideo)) { _ in
                openFile()
            }
            .onReceive(NotificationCenter.default.publisher(for: .subliftRequestExportSRT)) { _ in
                exportSRT()
            }
            .onAppear {
                applyWindowMetrics(hasVideo: workspace.currentVideoURL != nil)
                #if DEBUG
                EvidenceShot.settingsFixtureIfRequested()
                EvidenceShot.compactFixtureIfRequested()
                EvidenceShot.autoOpenIfRequested(workspace: workspace)
                EvidenceShot.inspectorFixtureIfRequested(workspace: workspace)
                EvidenceShot.regionFixtureIfRequested(workspace: workspace)
                EvidenceShot.extractFixtureIfRequested(workspace: workspace)
                EvidenceShot.selectFixtureIfRequested(workspace: workspace)
                EvidenceShot.entriesFixtureIfRequested(workspace: workspace)
                EvidenceShot.pendingFixtureIfRequested()
                EvidenceShot.scheduleIfRequested()
                #endif
            }
            .onChange(of: workspace.currentVideoURL) { url in
                applyWindowMetrics(hasVideo: url != nil)
            }
        }
    }

    // MARK: - Workspace Split（Video 60–65% / Transcript 35–40%，可调）

    private func workspaceSplit(url: URL) -> some View {
        GeometryReader { geometry in
            let inspectorWidth = workspace.isInspectorPresented ? WorkspaceLayout.inspectorReservedWidth : 0
            let split = WorkspaceLayout.splitWidths(
                containerWidth: geometry.size.width,
                inspectorWidth: inspectorWidth
            )
            HStack(spacing: 0) {
                WorkspaceSplitView(
                    initialVideoWidth: split.video,
                    leftMin: 400,
                    rightMin: WorkspaceLayout.transcriptMinWidth
                ) {
                    videoPane(url: url)
                } right: {
                    TranscriptPanel(
                        editor: workspace.editor,
                        onSeek: { ms in
                            if workspace.playerModel.loadFailed {
                                workspace.playerModel.fallbackSeek(toMs: ms)
                            } else {
                                workspace.playerModel.seek(toMs: ms)
                            }
                        },
                        onExport: exportSRT,
                        accessMode: workspace.transcriptAccessMode,
                        commands: workspace.commandAvailability,
                        currentMs: workspace.playerModel.currentMs,
                        readOnlyNotice: TranscriptReadOnlyNotice.text(for: workspace.state),
                        onSelectSubtitle: { workspace.selectSubtitle(id: $0) },
                        videoDurationMs: workspace.metadataLoader.metadata?.durationMs ?? 0
                    )
                }

                // Inspector 容器：先回收其预留位（内容按模式分发）。
                if workspace.isInspectorPresented {
                    Divider()
                    WorkspaceInspector(
                        mode: workspace.inspectorMode,
                        metadataLoader: workspace.metadataLoader,
                        playerModel: workspace.playerModel,
                        videoURL: workspace.currentVideoURL,
                        regionModel: workspace.regionModel,
                        extractor: workspace.extractor,
                        workspaceState: workspace.state,
                        editor: workspace.editor,
                        transcriptAccessMode: workspace.transcriptAccessMode,
                        activeExtractionConfiguration: workspace.activeExtractionConfiguration,
                        finalExtractionConfiguration: workspace.finalExtractionConfiguration,
                        onRedetectRegion: redetectRegionAtPlayhead
                    ) {
                        workspace.setInspectorPresented(false)
                    }
                }
            }
        }
    }

    // MARK: - Toolbar

    @ToolbarContentBuilder
    private var toolbarContent: some ToolbarContent {
        ToolbarItemGroup(placement: .navigation) {
            Button(action: openFile) {
                Label("打开视频", systemImage: "folder")
            }
            .disabled(!workspace.commandAvailability.canOpen)
            .help("打开 MP4、MOV 或 MKV 视频")
            .accessibilityLabel("打开视频")

            Button {
                openWindow(id: "task-center")
            } label: {
                Label("任务中心", systemImage: "rectangle.stack.badge.plus")
            }
            .help("打开批量任务中心（⌘⇧T）")
            .accessibilityLabel("任务中心")
        }

        ToolbarItem(placement: .principal) {
            if let url = workspace.currentVideoURL {
                Text(url.lastPathComponent)
                    .font(.headline)
                    .lineLimit(1)
                    .truncationMode(.middle)
                    .accessibilityLabel("当前视频：\(url.lastPathComponent)")
            }
        }

        ToolbarItemGroup(placement: .automatic) {
            if workspace.currentVideoURL != nil {
                Button {
                    if workspace.state == .regionEditing {
                        workspace.exitRegionEditing()
                    } else {
                        workspace.enterRegionEditing()
                    }
                } label: {
                    Label(workspace.state == .regionEditing ? "完成区域编辑" : "字幕区域", systemImage: "viewfinder")
                        .labelStyle(.titleAndIcon)
                }
                .disabled(!workspace.commandAvailability.canEditRegion && !workspace.commandAvailability.isRegionActive)
                .help(workspace.state == .regionEditing ? "完成区域编辑" : "进入字幕区域编辑")

                if workspace.commandAvailability.canExtract {
                    Button(action: requestExtraction) {
                        Label("提取字幕", systemImage: "text.viewfinder")
                            .labelStyle(.titleAndIcon)
                    }
                    .help("开始提取字幕")
                }

                if workspace.commandAvailability.canStop {
                    Button {
                        workspace.cancelExtraction()
                    } label: {
                        Label("停止", systemImage: "stop.fill")
                            .labelStyle(.titleAndIcon)
                    }
                    .help("停止当前提取")
                }

                Button(action: exportSRT) {
                    Label("导出 SRT", systemImage: "square.and.arrow.down")
                }
                .disabled(!workspace.commandAvailability.canExport)
                .help("导出 SRT 字幕文件")

                Button {
                    workspace.toggleInspector()
                } label: {
                    Label("Inspector", systemImage: "sidebar.trailing")
                }
                .disabled(!workspace.commandAvailability.canShowInspector)
                .help("显示或隐藏 Inspector")
            }
        }
    }

    // MARK: - Video Pane（复用现有业务组件）

    private func videoPane(url: URL) -> some View {
        GeometryReader { geometry in
            VStack(spacing: 0) {
                previewSection(height: PreviewLayout.height(
                    containerSize: geometry.size,
                    videoSize: CGSize(
                        width: workspace.metadataLoader.metadata?.width ?? 0,
                        height: workspace.metadataLoader.metadata?.height ?? 0
                    )
                ))
                Divider()
                VideoControlsView(model: workspace.playerModel)
                Divider()
                ExtractionProgressView(extractor: workspace.extractor)
                QuickExtractionSettingsBar(
                    state: workspace.state,
                    hasFinalEntries: workspace.hasFinalEntries,
                    finalConfiguration: workspace.finalExtractionConfiguration,
                    onRequestExtraction: requestExtraction
                )
                Spacer(minLength: 0)
            }
        }
    }

    @ViewBuilder
    private func previewSection(height: CGFloat) -> some View {
        let width = workspace.metadataLoader.metadata?.width ?? 0
        let videoHeight = workspace.metadataLoader.metadata?.height ?? 0

        PreviewRegionContainer(
            regionModel: workspace.regionModel,
            videoWidth: width,
            videoHeight: videoHeight,
            isRegionEditing: workspace.state == .regionEditing
        ) {
            if workspace.playerModel.loadFailed {
                unsupportedPreview
            } else {
                VideoPreview(model: workspace.playerModel)
            }
        }
        .frame(maxWidth: .infinity)
        .frame(height: height)
        .clipped()
    }

    // MARK: - 不支持的预览（mkv 等 AVPlayer 无法播放的格式）

    private var unsupportedPreview: some View {
        ZStack {
            Color.black

            if let image = workspace.playerModel.fallbackPreview {
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
            if workspace.playerModel.fallbackPreview != nil {
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

    // MARK: - 提取与导出

    /// 统一提取请求入口（Toolbar"提取字幕"与设置栏"重新提取"共用）。
    ///
    /// 先执行 Mock 归一化（持久化偏好同步回落）；已有最终结果时弹出替换确认，
    /// 确认后才调用 startExtraction；取消不改变字幕与配置。
    private func requestExtraction() {
        let engine = EngineCapability.normalizedSelection(
            defaultEngine,
            developerMode: developerMode
        )
        if defaultEngine != engine {
            defaultEngine = engine
        }
        let configuration = ExtractionRequestPolicy.normalizedConfiguration(
            engine: engine,
            quality: samplingQuality,
            developerMode: developerMode
        )

        if ExtractionRequestPolicy.requiresReplacementConfirmation(hasFinalEntries: workspace.hasFinalEntries) {
            pendingExtractionConfiguration = configuration
            showReplacementConfirmation = true
        } else {
            workspace.startExtraction(engine: configuration.engine, quality: configuration.quality)
        }
    }

    /// 在当前播放位置重跑 Vision 选区（用户可先 seek 到有字幕的画面）。
    private func redetectRegionAtPlayhead() {
        guard let meta = workspace.metadataLoader.metadata, let url = workspace.currentVideoURL else { return }
        let seconds = Double(workspace.playerModel.currentMs) / 1000.0
        Task {
            await workspace.regionModel.detectAt(
                url: url,
                videoWidth: meta.width,
                videoHeight: meta.height,
                sampleSeconds: seconds
            )
        }
    }

    private func exportSRT() {
        guard workspace.commandAvailability.canExport else { return }
        let entries = workspace.editor.exportEntries()
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

    /// Welcome 用紧凑窗；打开视频后升到工作台尺寸（已更大则不缩小）。
    private func applyWindowMetrics(hasVideo: Bool) {
        DispatchQueue.main.async {
            guard let window = NSApp.windows.first(where: {
                $0.isVisible && $0.identifier?.rawValue != "task-center"
            }) else { return }
            if hasVideo {
                window.contentMinSize = WorkspaceLayout.workbenchWindowMinSize
                let size = window.contentView?.bounds.size ?? .zero
                if size.width + 1 < WorkspaceLayout.workbenchWindowSize.width
                    || size.height + 1 < WorkspaceLayout.workbenchWindowSize.height {
                    window.setContentSize(WorkspaceLayout.workbenchWindowSize)
                }
            } else {
                window.contentMinSize = WorkspaceLayout.welcomeWindowMinSize
                window.setContentSize(WorkspaceLayout.welcomeWindowSize)
            }
        }
    }
}
