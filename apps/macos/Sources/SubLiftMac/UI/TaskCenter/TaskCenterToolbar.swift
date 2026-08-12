import SwiftUI

/// 08308：Task Center Toolbar（命令按 availability 启用；真实动作）。
struct TaskCenterToolbar: View {
    @ObservedObject var model: BatchQueueModel

    var body: some View {
        let queue = model.state
        let summary = TaskCenterPresentation.summary(for: queue)

        Button {
            // 08309 接线统一导入（文件+文件夹）；本 Feature 由空态 fileImporter 提供文件添加。
        } label: {
            Label("添加文件", systemImage: "plus")
        }
        .help("添加视频文件（空态视图提供完整选择器）")
        .disabled(true)

        Button {
            model.start()
        } label: {
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
}
