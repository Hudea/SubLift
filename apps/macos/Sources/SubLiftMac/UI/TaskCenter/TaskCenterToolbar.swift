import SwiftUI

/// 08308/08309：Task Center Toolbar（命令按 availability 启用；08309 接统一导入与冲突检查）。
struct TaskCenterToolbar: View {
    @ObservedObject var model: BatchQueueModel
    var onAddFiles: () -> Void
    var onAddFolder: () -> Void
    var onStart: () -> Void

    var body: some View {
        let queue = model.state
        let summary = TaskCenterPresentation.summary(for: queue)

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

        Button(action: onStart) {
            Label("开始", systemImage: "play.fill")
        }
        .help("开始批量提取")
        .disabled(!TaskCenterPresentation.canStart(queue))
        .accessibilityLabel("开始")

        Button {
            model.pauseAfterCurrent()
        } label: {
            Label("暂停", systemImage: "pause.fill")
        }
        .help("完成当前项后暂停")
        .disabled(!TaskCenterPresentation.canPauseAfterCurrent(queue))
        .accessibilityLabel("完成当前项后暂停")

        Button {
            model.resume()
        } label: {
            Label("恢复", systemImage: "play")
        }
        .help("恢复队列")
        .disabled(!TaskCenterPresentation.canResume(queue))
        .accessibilityLabel("恢复")

        Button {
            model.stop()
        } label: {
            Label("停止", systemImage: "stop.fill")
        }
        .help("停止调度")
        .disabled(!TaskCenterPresentation.canStop(queue))
        .accessibilityLabel("停止")

        Spacer()

        Text("\(summary.total) 个任务")
            .font(.caption)
            .foregroundStyle(.secondary)
            .accessibilityLabel("任务总数 \(summary.total)")
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
            Label(outputLocationLabel, systemImage: "folder")
        }
        .help("设置输出字幕的保存位置")
        .accessibilityLabel("输出位置")
    }

    private var outputLocationLabel: String {
        switch model.state.outputDestination {
        case .sidecar: "输出位置：视频旁边"
        case .publicRoot(let url): "输出位置：\(url.lastPathComponent)"
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
