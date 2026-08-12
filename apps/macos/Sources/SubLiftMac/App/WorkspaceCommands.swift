import SwiftUI

extension Notification.Name {
    /// 请求打开视频（菜单 → 视图层统一处理 Open Panel/校验/反馈）。
    static let subliftRequestOpenVideo = Notification.Name("sublift.requestOpenVideo")
    /// 请求导出 SRT（菜单 → 视图层统一处理 Save Panel/反馈）。
    static let subliftRequestExportSRT = Notification.Name("sublift.requestExportSRT")
}

/// `WorkspaceCommands` 与单元测试共用的命令路由。
///
/// Esc 由 App 级菜单 key equivalent 接收，不依赖当前 first responder 是否把事件传给
/// 某个嵌套 View 的 `onExitCommand`。
@MainActor
enum WorkspaceCommandRouter {
    static let escapeKeyCode: UInt16 = 53

    static func shouldInterceptEscape(
        keyCode: UInt16,
        workspaceState: WorkspaceState
    ) -> Bool {
        keyCode == escapeKeyCode && workspaceState == .regionEditing
    }

    @discardableResult
    static func performEscape(on workspace: WorkspaceModel) -> Bool {
        guard workspace.state == .regionEditing else { return false }
        workspace.exitRegionEditing()
        return true
    }
}

/// AppKit first-responder 之前的本地 keyDown monitor。
///
/// SwiftUI 的 `.onExitCommand` 与菜单 key equivalent 都可能被 `NSTextField` 先消费。
/// 本对象由根视图中的 `NSViewRepresentable` coordinator 持有，安装幂等、拆除幂等，
/// 且只消费 Region Editing 状态下的 Esc；其余事件原样返回。
@MainActor
final class WorkspaceEscapeKeyMonitor {
    typealias EventHandler = (NSEvent) -> NSEvent?
    typealias Installer = (@escaping EventHandler) -> Any
    typealias Remover = (Any) -> Void

    private weak var workspace: WorkspaceModel?
    private var monitorToken: Any?
    private let installer: Installer
    private let remover: Remover

    init(
        installer: @escaping Installer = { handler in
            NSEvent.addLocalMonitorForEvents(matching: .keyDown, handler: handler) as Any
        },
        remover: @escaping Remover = { NSEvent.removeMonitor($0) }
    ) {
        self.installer = installer
        self.remover = remover
    }

    deinit {
        if let monitorToken {
            remover(monitorToken)
        }
    }

    var isInstalled: Bool { monitorToken != nil }

    func installIfNeeded(workspace: WorkspaceModel) {
        self.workspace = workspace
        guard monitorToken == nil else { return }
        monitorToken = installer { [weak self] event in
            guard let self, let workspace = self.workspace else { return event }
            guard WorkspaceCommandRouter.shouldInterceptEscape(
                keyCode: event.keyCode,
                workspaceState: workspace.state
            ) else { return event }
            return WorkspaceCommandRouter.performEscape(on: workspace) ? nil : event
        }
    }

    func uninstall() {
        guard let monitorToken else { return }
        remover(monitorToken)
        self.monitorToken = nil
        workspace = nil
    }
}

/// 将 AppKit key monitor 生命周期绑定到当前 Workspace 窗口视图。
struct WorkspaceEscapeKeyMonitorView: NSViewRepresentable {
    let workspace: WorkspaceModel

    func makeCoordinator() -> Coordinator {
        Coordinator()
    }

    func makeNSView(context: Context) -> NSView {
        context.coordinator.monitor.installIfNeeded(workspace: workspace)
        return NSView(frame: .zero)
    }

    func updateNSView(_ nsView: NSView, context: Context) {
        context.coordinator.monitor.installIfNeeded(workspace: workspace)
    }

    static func dismantleNSView(_ nsView: NSView, coordinator: Coordinator) {
        coordinator.monitor.uninstall()
    }

    @MainActor
    final class Coordinator {
        let monitor = WorkspaceEscapeKeyMonitor()
    }
}

/// 10104：Workspace 统一菜单命令。
///
/// File / Edit / Playback / View 菜单与 Toolbar 共用 WorkspaceModel 的 action 与 availability；
/// 菜单不各自实现业务逻辑。需要系统面板/Alert 的命令（Open、Export）通过
/// NotificationCenter 交回视图层统一处理。
struct WorkspaceCommands: Commands {
    @ObservedObject var workspace: WorkspaceModel

    var body: some Commands {
        // File：Open（替换 New Item 位）
        CommandGroup(replacing: .newItem) {
            Button("打开视频…") {
                NotificationCenter.default.post(name: .subliftRequestOpenVideo, object: nil)
            }
            .keyboardShortcut("o", modifiers: .command)
            .disabled(!workspace.commandAvailability.canOpen)
        }

        // File：Export SRT（Save Item 之后）
        CommandGroup(after: .saveItem) {
            Button("导出 SRT…") {
                NotificationCenter.default.post(name: .subliftRequestExportSRT, object: nil)
            }
            .keyboardShortcut("e", modifiers: [.command, .shift])
            .disabled(!workspace.commandAvailability.canExport)
        }

        // Edit：Split / Merge（系统文本编辑命令之后）
        CommandGroup(after: .pasteboard) {
            Divider()
            Button("拆分字幕") {
                guard let id = workspace.editor.selectedId,
                      let idx = workspace.editor.entries.firstIndex(where: { $0.id == id }) else { return }
                workspace.editor.split(at: idx)
            }
            .keyboardShortcut("s", modifiers: [.command, .option])
            .disabled(!workspace.commandAvailability.canSplit)

            Button("与下一条合并") {
                guard let id = workspace.editor.selectedId,
                      let idx = workspace.editor.entries.firstIndex(where: { $0.id == id }) else { return }
                workspace.editor.merge(at: idx)
            }
            .keyboardShortcut("m", modifiers: [.command, .option])
            .disabled(!workspace.commandAvailability.canMerge)
        }

        // Playback：Play/Pause（Space 统一到菜单）
        CommandMenu("Playback") {
            Button(workspace.playerModel.isPlaying ? "暂停" : "播放") {
                workspace.playerModel.togglePlay()
            }
            .keyboardShortcut(.space, modifiers: [])
            .disabled(!canPlayPause)
        }

        // View：Region 与 Inspector
        CommandGroup(after: .toolbar) {
            Button("退出区域编辑") {
                WorkspaceCommandRouter.performEscape(on: workspace)
            }
            .keyboardShortcut(.escape, modifiers: [])
            .disabled(workspace.state != .regionEditing)

            Button(workspace.state == .regionEditing ? "完成区域编辑" : "编辑字幕区域") {
                if workspace.state == .regionEditing {
                    workspace.exitRegionEditing()
                } else {
                    workspace.enterRegionEditing()
                }
            }
            .keyboardShortcut("r", modifiers: [.command, .option])
            .disabled(!workspace.commandAvailability.canEditRegion && !workspace.commandAvailability.isRegionActive)

            Button(workspace.isInspectorPresented ? "隐藏 Inspector" : "显示 Inspector") {
                workspace.toggleInspector()
            }
            .keyboardShortcut("i", modifiers: .command)
            .disabled(!workspace.commandAvailability.canShowInspector)
        }
    }

    /// 播放/暂停可用：已加载视频且非 fallback（mkv 不支持播放）。
    private var canPlayPause: Bool {
        workspace.currentVideoURL != nil && !workspace.playerModel.loadFailed
    }
}
