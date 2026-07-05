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
    @State private var useMockEngine = false
    @State private var showFfmpegMissingAlert = false

    var body: some View {
        VStack(spacing: 0) {
            if let url = videoURL {
                VStack(spacing: 0) {
                    if playerModel.loadFailed {
                        unsupportedPreview
                    } else {
                        VideoPreview(model: playerModel)
                    }
                    Divider()
                    VideoControlsView(model: playerModel)
                    Divider()
                    extractionBar(url: url)
                }
                .frame(maxHeight: .infinity)

                if !extractor.entries.isEmpty {
                    entriesList
                }
            } else {
                emptyState
            }
        }
        .frame(minWidth: 800, minHeight: 480)
        .onChange(of: videoURL) { newURL in
            playerModel.url = newURL
            if let url = newURL, url.pathExtension.lowercased() == "mkv",
               FfmpegDetector.detect() == nil {
                showFfmpegMissingAlert = true
            }
        }
        .alert("需要 ffmpeg", isPresented: $showFfmpegMissingAlert) {
            Button("确定", role: .cancel) { }
        } message: {
            Text("mkv 视频需要 ffmpeg 支持。请先安装：\nbrew install ffmpeg\n\n安装后重新打开视频。")
        }
    }

    // MARK: - entries 列表（只读，feat-021 才做编辑）

    private var entriesList: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack {
                Text("识别结果（\(extractor.entries.count) 条）")
                    .font(.headline)
                Spacer()
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 6)

            Divider()

            ScrollView {
                LazyVStack(alignment: .leading, spacing: 4) {
                    ForEach(Array(extractor.entries.enumerated()), id: \.offset) { _, entry in
                        HStack(alignment: .top, spacing: 8) {
                            Text("\(TimeFormatter.formatMs(entry.startMs))")
                                .font(.system(.caption, design: .monospaced))
                                .foregroundStyle(.secondary)
                                .frame(width: 110, alignment: .leading)
                            Text(entry.text)
                                .textSelection(.enabled)
                        }
                        .padding(.horizontal, 12)
                        .padding(.vertical, 2)
                    }
                }
                .padding(.vertical, 4)
            }
            .frame(maxHeight: 160)
        }
        .background(Color(nsColor: .textBackgroundColor))
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
