import SwiftUI

/// 08308/08309：Task Center Toolbar（命令按 availability 启用；08309 接统一导入与冲突检查）。
struct TaskCenterToolbar: View {
    @ObservedObject var model: BatchQueueModel
    var onAddFiles: () -> Void
    var onAddFolder: () -> Void
    var onStart: () -> Void

    var body: some View {
        let transport = TaskCenterPresentation.transport(
            for: model.state,
            reason: model.pauseReason
        )

        Button(action: onAddFiles) {
            Label("添加文件", systemImage: "plus")
        }
        .help("添加视频文件")
        .accessibilityLabel("添加文件")

        Button(action: onAddFolder) {
            Label("添加文件夹", systemImage: "folder.badge.plus")
        }
        .help("添加文件夹（扫描其中视频）")
        .accessibilityLabel("添加文件夹")

        outputLocationMenu

        primaryQueueButton(transport)

        if transport.showsCancelCurrent {
            Button(role: .destructive) {
                model.stop()
            } label: {
                Label("取消当前", systemImage: "stop.fill")
            }
            .labelStyle(.titleAndIcon)
            .help("取消当前任务并暂停队列")
            .accessibilityLabel("取消当前任务并暂停队列")
        }
    }

    @ViewBuilder
    private func primaryQueueButton(_ transport: TaskCenterPresentation.QueueTransport) -> some View {
        if transport.primaryIsPressed {
            queuePrimaryControl(transport)
                .buttonStyle(.borderedProminent)
        } else {
            queuePrimaryControl(transport)
        }
    }

    private func queuePrimaryControl(_ transport: TaskCenterPresentation.QueueTransport) -> some View {
        Button {
            switch transport.primaryAction {
            case .start: onStart()
            case .resume: model.resume()
            case .pauseAfterCurrent: model.pauseAfterCurrent()
            case .clearPauseRequest: model.clearPauseRequest()
            case .none: break
            }
        } label: {
            Label(transport.primaryTitle, systemImage: transport.primarySymbolName)
        }
        .labelStyle(.titleAndIcon)
        .help(transport.primaryHelp)
        .disabled(!transport.primaryEnabled)
        .accessibilityLabel(transport.primaryTitle)
        .accessibilityHint(transport.primaryHelp)
    }

    // MARK: - 08511 输出位置菜单

    private var outputLocationMenu: some View {
        Menu {
            Button {
                model.setOutputDestination(.sidecar)
            } label: {
                if case .sidecar = model.state.outputDestination {
                    Label("视频旁边", systemImage: "checkmark")
                } else {
                    Text("视频旁边")
                }
            }
            Button {
                choosePublicRootFolder()
            } label: {
                if case .publicRoot = model.state.outputDestination {
                    Label("选择文件夹…", systemImage: "checkmark")
                } else {
                    Text("选择文件夹…")
                }
            }
        } label: {
            Label(outputLocationLabel, systemImage: "square.and.arrow.down")
                .labelStyle(.titleAndIcon)
        }
        .help("字幕默认写在每个视频旁边，也可改到指定文件夹")
        .accessibilityLabel(outputLocationLabel)
    }

    private var outputLocationLabel: String {
        switch model.state.outputDestination {
        case .sidecar: "导出位置"
        case .publicRoot(let url): "导出位置：\(url.lastPathComponent)"
        }
    }

    private func choosePublicRootFolder() {
        let panel = NSOpenPanel()
        panel.canChooseFiles = false
        panel.canChooseDirectories = true
        panel.allowsMultipleSelection = false
        panel.message = "选择字幕输出文件夹"
        panel.prompt = "选择"
        guard panel.runModal() == .OK, let url = panel.url else { return }
        model.setOutputDestination(.publicRoot(url))
    }
}
