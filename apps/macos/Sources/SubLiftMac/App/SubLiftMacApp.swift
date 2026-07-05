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

    var body: some View {
        VStack(spacing: 0) {
            if videoURL != nil {
                VideoPreview(model: playerModel)
                Divider()
                VideoControlsView(model: playerModel)
            } else {
                emptyState
            }
        }
        .frame(minWidth: 640, minHeight: 360)
        .onChange(of: videoURL) { newURL in
            playerModel.url = newURL
        }
    }

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
