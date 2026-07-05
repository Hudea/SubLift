import SwiftUI
import UniformTypeIdentifiers

@main
struct SubLiftMacApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var appDelegate

    var body: some Scene {
        WindowGroup {
            ContentView()
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
    @State private var videoURL: URL?
    @StateObject private var playerModel = PlayerModel()
    @StateObject private var extractor = SubtitleExtractor()
    @StateObject private var editor = SubtitleEditor()
    @StateObject private var metadataLoader = VideoMetadataLoader()
    @State private var useMockEngine = false
    @State private var showFfmpegMissingAlert = false

    var body: some View {
        Group {
            if let url = videoURL {
                HSplitView {
                    // 左侧：预览 + 控制 + 元数据 + 提取
                    VStack(spacing: 0) {
                        if playerModel.loadFailed {
                            unsupportedPreview
                        } else {
                            VideoPreview(model: playerModel)
                        }
                        Divider()
                        VideoControlsView(model: playerModel)
                        Divider()
                        metadataBar
                        Divider()
                        extractionBar(url: url)
                    }
                    .frame(minWidth: 400)

                    // 右侧：字幕列表 + 编辑
                    SubtitleList(editor: editor) { ms in
                        if playerModel.loadFailed {
                            playerModel.fallbackSeek(toMs: ms)
                        } else {
                            playerModel.seek(toMs: ms)
                        }
                    }
                        .frame(minWidth: 360)
                }
            } else {
                emptyState
            }
        }
        .frame(minWidth: 800, minHeight: 480)
        .dropDestination(for: URL.self) { items, _ in
            guard let url = items.first else { return false }
            videoURL = url
            return true
        }
        .onChange(of: videoURL) { newURL in
            playerModel.url = newURL
            editor.clear()
            if let url = newURL {
                Task { await metadataLoader.load(url: url) }
            } else {
                metadataLoader.clear()
            }
            if let url = newURL, url.pathExtension.lowercased() == "mkv",
               FfmpegDetector.detect() == nil {
                showFfmpegMissingAlert = true
            }
        }
        .onChange(of: extractor.status) { newStatus in
            if case .done = newStatus, !extractor.entries.isEmpty {
                editor.load(extractor.entries)
            }
        }
        .onChange(of: playerModel.currentMs) { newMs in
            editor.updateCurrent(atMs: newMs)
        }
        .alert("需要 ffmpeg", isPresented: $showFfmpegMissingAlert) {
            Button("确定", role: .cancel) { }
        } message: {
            Text("mkv 视频需要 ffmpeg 支持。请先安装：\nbrew install ffmpeg\n\n安装后重新打开视频。")
        }
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
                    extractor.extract(videoURL: url, engine: useMockEngine ? "mock" : "vision")
                } label: {
                    Label("提取字幕", systemImage: "text.viewfinder")
                }
                .buttonStyle(.borderedProminent)
                .disabled(extractor.isRunning)

                if extractor.isRunning {
                    Button("取消") { extractor.cancel() }
                        .buttonStyle(.bordered)
                }

                Toggle("Mock 引擎", isOn: $useMockEngine)
                    .toggleStyle(.checkbox)
                    .disabled(extractor.isRunning)

                Spacer()

                statusLabel
            }

            if case .sampling(let progress, _, _) = extractor.status {
                ProgressView(value: progress)
                    .progressViewStyle(.linear)
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
            Text("启动 Python 服务...").foregroundStyle(.orange)
        case .sampling(_, let frameCount, let totalFrames):
            Text("抽帧中 \(frameCount)/\(totalFrames)")
                .foregroundStyle(.blue)
                .font(.system(.body, design: .monospaced))
        case .processing:
            Text("OCR 处理中...").foregroundStyle(.orange)
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

    // MARK: - 空状态

    private var emptyState: some View {
        VStack(spacing: 16) {
            Image(systemName: "film")
                .font(.system(size: 48))
                .foregroundStyle(.secondary)
            Text("SubLift")
                .font(.title)
            Text("硬字幕提取工具")
                .font(.subheadline)
                .foregroundStyle(.secondary)
            Button("打开视频") { openFile() }
                .buttonStyle(.borderedProminent)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .background(Color(nsColor: .windowBackgroundColor))
    }

    private func openFile() {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [.movie]
        panel.allowsMultipleSelection = false
        panel.canChooseDirectories = false
        if panel.runModal() == .OK {
            videoURL = panel.url
        }
    }
}
