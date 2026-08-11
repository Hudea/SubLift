import SwiftUI

extension Notification.Name {
    /// 请求打开视频（菜单 → 视图层统一处理 Open Panel/校验/反馈）。
    static let subliftRequestOpenVideo = Notification.Name("sublift.requestOpenVideo")
    /// 请求导出 SRT（菜单 → 视图层统一处理 Save Panel/反馈）。
    static let subliftRequestExportSRT = Notification.Name("sublift.requestExportSRT")
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
