import SwiftUI
import UniformTypeIdentifiers

/// 08308：Task Center 主视图（原生 Table/Toolbar/Detail/Summary）。
struct TaskCenterView: View {
    @ObservedObject var model: BatchQueueModel
    @State private var selection: UUID?
    @State private var isImportingFiles = false

    var body: some View {
        let summary = TaskCenterPresentation.summary(for: model.state)
        VStack(spacing: 0) {
            summaryBar
            Divider()
            // 条件分支（if/else）在 NSWindow hosting 离屏渲染（cacheDisplay）实测黑屏——
            // 改用 ZStack 同时渲染表格与空态 overlay，避免 SwiftUI 分支切换问题。
            ZStack {
                TaskTableView(tasks: model.state.tasks, selection: $selection)
                if summary.total == 0 {
                    emptyState
                }
            }
            if summary.total > 0 {
                Divider()
                if let selectedID = selection,
                   let task = model.state.tasks.first(where: { $0.id == selectedID }) {
                    TaskDetailView(task: task)
                }
            }
        }
        .toolbar { TaskCenterToolbar(model: model) }
        .fileImporter(
            isPresented: $isImportingFiles,
            allowedContentTypes: [.movie, .video, .mpeg4Movie],
            allowsMultipleSelection: true
        ) { result in
            if case .success(let urls) = result {
                addVideos(urls)
            }
        }
        .alert("队列恢复失败", isPresented: .init(
            get: { model.recoveryErrorMessage != nil },
            set: { if !$0 { model.dismissRecoveryError() } }
        )) {
            Button("确定", role: .cancel) { model.dismissRecoveryError() }
        } message: {
            Text(model.recoveryErrorMessage ?? "")
        }
    }

    /// 空态（B01）：单一"添加文件"主动作 + "添加文件夹"次动作 + 本机处理说明。
    private var emptyState: some View {
        VStack(spacing: 12) {
            Image(systemName: "rectangle.stack.badge.plus")
                .font(.system(size: 44))
                .foregroundStyle(.secondary)
            Text("添加视频开始批量提取")
                .font(.title3.weight(.medium))
            Button("添加文件") {
                isImportingFiles = true
            }
            .buttonStyle(.borderedProminent)
            .accessibilityLabel("添加文件")
            Button("添加文件夹") {
                // 文件夹导入（Scanner 拒绝摘要）由 08309 接线；本 Feature 提供次动作占位。
            }
            .disabled(true)
            .help("文件夹导入在 08309 提供")
            Text("视频在本机处理，不会上传。")
                .font(.caption)
                .foregroundStyle(.secondary)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    /// 真实文件选择 → 构造等待任务（engine/quality 用默认并归一化）→ 入队。
    private func addVideos(_ urls: [URL]) {
        let tasks = urls.map { url in
            BatchTask.make(
                sourceURL: url.standardizedFileURL,
                engine: .vision,
                quality: .fast,
                developerMode: false
            )
        }
        model.addTasks(tasks)
    }

    /// 汇总条（真实计数）。
    private var summaryBar: some View {
        let summary = TaskCenterPresentation.summary(for: model.state)
        return HStack(spacing: 16) {
            Label("\(summary.total)", systemImage: "list.bullet")
                .labelStyle(.titleAndIcon)
                .accessibilityLabel("任务总数 \(summary.total)")
            Text("等待 \(summary.waiting)")
            Text("进行中 \(summary.active)")
            Text("完成 \(summary.completed)")
            Text("失败 \(summary.failed)")
            Text("取消 \(summary.cancelled)")
            Text("跳过 \(summary.skipped)")
            Spacer()
        }
        .font(.caption)
        .foregroundStyle(.secondary)
        .padding(.horizontal, 12)
        .padding(.vertical, 6)
        .accessibilityElement(children: .combine)
    }
}
