import SwiftUI
import UniformTypeIdentifiers

@main
struct SubLiftMacApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) var appDelegate
    @StateObject private var workspace = WorkspaceModel()
    @StateObject private var batchQueueModel = BatchQueueModel(
        runner: BatchExtractionRunner(),
        repository: BatchQueueRepository()
    )

    var body: some Scene {
        WindowGroup {
            WorkspaceRootView(workspace: workspace)
                .onAppear {
                    #if DEBUG
                    EvidenceShot.taskCenterFixtureIfRequested(model: batchQueueModel)
                    #endif
                }
        }
        .defaultSize(width: 1280, height: 800)
        .commands {
            WorkspaceCommands(workspace: workspace)
            // 08308：Task Center 入口挂主场景（Workspace 菜单），⌘⇧T 冷启动可用。
            TaskCenterCommands(model: batchQueueModel)
        }

        // 08308：独立 Task Center Window（App scene 层唯一 BatchQueueModel）。
        WindowGroup("任务中心", id: "task-center") {
            TaskCenterView(model: batchQueueModel)
                .frame(minWidth: 960, minHeight: 600)
        }
        .defaultSize(width: 1280, height: 800)
        .commands {
            TaskCenterCommands(model: batchQueueModel)
        }

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
