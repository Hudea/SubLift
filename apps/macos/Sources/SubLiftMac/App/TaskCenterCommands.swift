import SwiftUI

/// 08308：Task Center 命令（⌘⇧T 打开独立窗口）。
///
/// macOS 13 规范路径：`@Environment(\.openWindow)` + `openWindow(id:)`
/// 创建并聚焦 WindowGroup("任务中心", id: "task-center") 场景窗口。
struct TaskCenterCommands: Commands {
    @ObservedObject var model: BatchQueueModel
    @Environment(\.openWindow) private var openWindow

    var body: some Commands {
        CommandGroup(after: .toolbar) {
            Button("打开任务中心") {
                openWindow(id: "task-center")
            }
            .keyboardShortcut("t", modifiers: [.command, .shift])
        }
    }
}
