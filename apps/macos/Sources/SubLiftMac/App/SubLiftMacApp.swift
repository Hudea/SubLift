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

    var body: some View {
        VStack(spacing: 0) {
            if let url = videoURL {
                VStack(spacing: 0) {
                    VideoPreview(model: playerModel)
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
