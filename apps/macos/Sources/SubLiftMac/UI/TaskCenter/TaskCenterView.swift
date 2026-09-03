import SwiftUI
import UniformTypeIdentifiers

/// 08308/08309：Task Center 主视图（Table/筛选/多选操作/统一导入/确认/Finder）。
struct TaskCenterView: View {
    @ObservedObject var model: BatchQueueModel
    @State private var selection: Set<UUID> = []
    @State private var importKind: TaskCenterImportKind = .files
    @State private var isImporting = false
    @State private var isDropTargeted = false

    // 确认/错误状态
    @State private var showDeleteConfirmation = false
    @State private var showConflictConfirmation = false
    @State private var showFinderError = false
    @State private var revealErrorURL: URL?
    @State private var scanReasonsExpanded = false
    /// 冲突确认后走 startSingle 而不是整队 start。
    @State private var startSinglePendingID: UUID?
    /// 08511：窗口级宽度（EvidenceShot fixture 通过环境变量注入；真实窗口由 onAppear 采样）。
    @State private var windowWidth: CGFloat = 1280

    private var selectedTasks: [BatchTask] {
        model.state.tasks.filter { selection.contains($0.id) }
    }

    /// 选中中可删除（waiting）的数量——删除按钮与确认文案共用。
    private var removableSelectionCount: Int {
        selectedTasks.filter { BatchTaskCommandAvailability.canRemove($0.status) }.count
    }

    private var visibleColumnsForTable: [TaskTableColumn] {
        #if DEBUG
        if let envWidth = ProcessInfo.processInfo.environment["SUBLIFT_EVIDENCE_WINDOW_WIDTH"],
           let w = Double(envWidth) {
            return TaskCenterPresentation.visibleColumns(forWidth: CGFloat(w))
        }
        #endif
        return TaskCenterPresentation.visibleColumns(forWidth: windowWidth)
    }

    var body: some View {
        let emptyCanvas = TaskCenterPresentation.shouldShowEmptyCanvas(for: model.state)
        VStack(spacing: 0) {
            summaryBar
            Divider()
            if !emptyCanvas {
                filterBar
            }
            if model.lastScanSummary != nil {
                Divider()
                scanSummaryBanner
            }
            if emptyCanvas {
                emptyCanvasDropZone
            } else {
                Divider()
                TaskTableView(tasks: model.filteredTasks, selection: $selection, visibleColumns: visibleColumnsForTable)
                    .contextMenu(forSelectionType: UUID.self) { ids in
                        if let id = ids.first,
                           let task = model.state.tasks.first(where: { $0.id == id }) {
                            Button("在 Finder 中显示源文件") { revealSource(task) }
                            if task.outputURL != nil {
                                Button("在 Finder 中显示字幕") { revealOutput(task) }
                            }
                            if BatchTaskCommandAvailability.canCancel(task.status) {
                                Button("取消任务") { _ = model.cancel(task.id) }
                            }
                            if BatchTaskCommandAvailability.canRetry(task.status) {
                                Button("重试任务") { _ = model.retry(task.id) }
                            }
                            if BatchTaskCommandAvailability.canStartSingle(task.status) {
                                Button("启动此任务") { requestStartSingle(task.id) }
                            }
                            if BatchTaskCommandAvailability.canRemove(task.status) {
                                Button("删除任务", role: .destructive) {
                                    selection = [task.id]
                                    showDeleteConfirmation = true
                                }
                            }
                        }
                    }
                    .onDeleteCommand {
                        if removableSelectionCount > 0 {
                            showDeleteConfirmation = true
                        }
                    }
                Divider()
                if selection.count > 1 {
                    selectionActionBar
                }
                if let selectedID = selection.count == 1 ? selection.first : nil,
                   let task = model.state.tasks.first(where: { $0.id == selectedID }) {
                    Divider()
                    let inspector = TaskCenterPresentation.inspectorModel(
                        for: task,
                        fileExists: task.outputURL.map { FileManager.default.fileExists(atPath: $0.path) } ?? false,
                        planningError: model.planningErrors[task.id]
                    )
                    // .id(task.id)：切换选中时重建详情（避免 @State 编辑态跨任务残留）。
                    TaskDetailView(
                        task: task,
                        inspectorModel: inspector,
                        onReplaceConfiguration: { configuration in
                            _ = model.replaceTaskConfiguration(task.id, configuration)
                        },
                        onCancel: { _ = model.cancel(task.id) },
                        onRetry: { _ = model.retry(task.id) },
                        onRemove: { _ = model.remove(task.id) },
                        onMoveUp: { moveSingleTask(.up, id: task.id) },
                        onMoveDown: { moveSingleTask(.down, id: task.id) },
                        onStartSingle: { requestStartSingle(task.id) }
                    )
                    .id(task.id)
                }
            }
        }
        .toolbar {
            TaskCenterToolbar(
                model: model,
                onAddFiles: { beginImport(.files) },
                onAddFolder: { beginImport(.folder) },
                onStart: requestStart
            )
        }
        .tint(fixtureAccentColor)
        .onDrop(of: [.movie, .video, .mpeg4Movie, .folder, .item], isTargeted: $isDropTargeted) { providers in
            importDroppedProviders(providers)
        }
        .fileImporter(
            isPresented: $isImporting,
            allowedContentTypes: importKind == .folder ? [.folder] : VideoImportPolicy.openPanelContentTypes,
            allowsMultipleSelection: importKind != .folder
        ) { result in
            if case .success(let urls) = result {
                model.importInputs(urls)
            }
        }
        // 删除确认（危险操作；键盘焦点安全：alert 默认按钮为取消）。
        .alert("删除选中任务？", isPresented: $showDeleteConfirmation) {
            Button("取消", role: .cancel) {}
            Button("删除", role: .destructive) { performDeleteSelection() }
        } message: {
            Text("将从队列移除 \(removableSelectionCount) 个任务。已写出的字幕文件不会被删除。")
        }
        // 输出冲突集中确认（开始前；取消不启动、不覆盖任何输出）。
        .alert("替换现有字幕？", isPresented: $showConflictConfirmation) {
            Button("取消", role: .cancel) {
                startSinglePendingID = nil
                model.cancelStart()
            }
            Button("替换并开始", role: .destructive) {
                if let id = startSinglePendingID {
                    model.confirmOutputConflictsAndStartSingle(id)
                    startSinglePendingID = nil
                } else {
                    model.confirmOutputConflictsAndStart()
                }
            }
        } message: {
            Text("以下字幕文件已存在，将被覆盖：\n\(conflictMessage)")
        }
        // Finder 定位错误。
        .alert("找不到文件", isPresented: $showFinderError) {
            Button("好", role: .cancel) {}
        } message: {
            Text(revealErrorURL?.path ?? "")
        }
        .alert("队列恢复失败", isPresented: .init(
            get: { model.recoveryErrorMessage != nil },
            set: { if !$0 { model.dismissRecoveryError() } }
        )) {
            Button("确定", role: .cancel) { model.dismissRecoveryError() }
        } message: {
            Text(model.recoveryErrorMessage ?? "")
        }
        .background {
            Button("删除选中任务") {
                if removableSelectionCount > 0 { showDeleteConfirmation = true }
            }
            .keyboardShortcut(.delete, modifiers: [])
            .opacity(0)
            .accessibilityHidden(true)
            Button("启动选中任务") {
                if let id = selection.count == 1 ? selection.first : nil {
                    requestStartSingle(id)
                }
            }
            .keyboardShortcut(.return, modifiers: [])
            .opacity(0)
            .accessibilityHidden(true)
        }
        .onAppear {
            // 真实窗口：采样实际宽度（fixture 通过 SUBLIFT_EVIDENCE_WINDOW_WIDTH 注入）。
            #if DEBUG
            if let envWidth = ProcessInfo.processInfo.environment["SUBLIFT_EVIDENCE_WINDOW_WIDTH"],
               let w = Double(envWidth) {
                windowWidth = CGFloat(w)
            } else if let window = NSApp.keyWindow ?? NSApp.windows.first(where: { $0.isVisible }) {
                windowWidth = window.frame.width
            }
            if ProcessInfo.processInfo.environment["SUBLIFT_EVIDENCE_SELECT"] == "1",
               let first = model.state.tasks.first {
                selection = [first.id]
            }
            #endif
        }
    }

    // MARK: - 顶部栏

    /// 回车 / 右键：只跑选中的 waiting 任务。
    private func requestStartSingle(_ taskID: UUID) {
        guard TaskCenterPresentation.canStartSingle(model.state, taskID: taskID) else { return }
        startSinglePendingID = taskID
        if model.startSingle(taskID) {
            startSinglePendingID = nil
            return
        }
        if !model.outputConflicts.isEmpty {
            showConflictConfirmation = true
        } else {
            startSinglePendingID = nil
        }
    }

    /// 开始请求：先规划输出（08309）——有冲突 → 集中确认（消费 TaskCenterInteraction.replaceDecision）；无冲突直接启动。
    private func requestStart() {
        startSinglePendingID = nil
        model.prepareOutputPlan()
        let decision = TaskCenterInteraction.replaceDecision(
            confirming: model.outputConflicts.isEmpty,
            conflicts: model.outputConflicts
        )
        switch decision {
        case .replaceAndStart:
            model.confirmOutputConflictsAndStart()
        case .cancel:
            showConflictConfirmation = true
        }
    }

    /// B09 fixture：非默认 Accent（DEBUG-only，SUBLIFT_EVIDENCE_ACCENT=1 → orange）。
    private var fixtureAccentColor: Color? {
        #if DEBUG
        if ProcessInfo.processInfo.environment["SUBLIFT_EVIDENCE_ACCENT"] == "1" {
            return .orange
        }
        #endif
        return nil
    }

    private var summaryBar: some View {
        let summary = TaskCenterPresentation.summary(for: model.state)
        let transport = TaskCenterPresentation.transport(
            for: model.state,
            reason: model.pauseReason
        )
        return HStack(spacing: 16) {
            Label("\(summary.total)", systemImage: "list.bullet")
                .labelStyle(.titleAndIcon)
                .accessibilityLabel(TaskCenterAccessibility.summaryLabel(
                    total: summary.total, waiting: summary.waiting, active: summary.active,
                    completed: summary.completed, failed: summary.failed,
                    cancelled: summary.cancelled, skipped: summary.skipped,
                    queueStatus: transport.statusText
                ))
            Text("等待 \(summary.waiting)")
            Text("进行中 \(summary.active)")
            Text("完成 \(summary.completed)")
            Text("失败 \(summary.failed)")
            Text("取消 \(summary.cancelled)")
            Text("跳过 \(summary.skipped)")
            Spacer()
            if !transport.statusText.isEmpty {
                Text(transport.statusText)
                    .lineLimit(1)
                    .truncationMode(.middle)
            }
        }
        .font(.caption)
        .foregroundStyle(.secondary)
        .padding(.horizontal, 12)
        .padding(.vertical, 6)
        .accessibilityElement(children: .combine)
    }

    /// 筛选栏（08309：搜索/状态筛选只改变投影；08511：空队列时 disabled）。
    private var filterBar: some View {
        let emptyCanvas = TaskCenterPresentation.shouldShowEmptyCanvas(for: model.state)
        return HStack(spacing: 8) {
            Image(systemName: "magnifyingglass")
                .foregroundStyle(.secondary)
            TextField("搜索文件或位置", text: $model.searchText)
                .textFieldStyle(.roundedBorder)
                .frame(maxWidth: 240)
                .accessibilityLabel("搜索文件")
            Picker("状态", selection: $model.statusFilter) {
                ForEach(BatchTaskStatusFilter.allCases, id: \.self) { filter in
                    Text(filterDisplayName(filter)).tag(filter)
                }
            }
            .pickerStyle(.segmented)
            .frame(maxWidth: 320)
            .accessibilityLabel("状态筛选")
            Spacer()
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 6)
        .disabled(emptyCanvas)
    }

    private func filterDisplayName(_ filter: BatchTaskStatusFilter) -> String {
        switch filter {
        case .all: "全部"
        case .waiting: "等待"
        case .running: "进行中"
        case .completed: "完成"
        case .failed: "失败"
        case .cancelled: "取消"
        case .skipped: "跳过"
        }
    }

    /// B02 扫描摘要横幅（接受/拒绝计数 + 逐项原因，可展开；不自动开始）。
    private var scanSummaryBanner: some View {
        HStack(alignment: .top, spacing: 8) {
            Image(systemName: "checkmark.circle")
                .foregroundStyle(.green)
            VStack(alignment: .leading, spacing: 2) {
                Text("扫描完成：接受 \(model.lastScanSummary?.accepted.count ?? 0) 个，跳过 \(model.lastScanSummary?.skipped ?? 0) 个，拒绝 \(model.lastScanSummary?.rejected.count ?? 0) 个")
                    .font(.caption)
                if scanReasonsExpanded {
                    ForEach(model.lastScanSummary?.rejected ?? [], id: \.url) { rejection in
                        Text("· \(rejection.url.lastPathComponent)：\(rejection.reason.localizedDescription)")
                            .font(.caption2)
                            .foregroundStyle(.secondary)
                    }
                }
            }
            Spacer()
            Button(scanReasonsExpanded ? "收起" : "查看原因") { scanReasonsExpanded.toggle() }
                .font(.caption)
                .accessibilityLabel(scanReasonsExpanded ? "收起拒绝原因" : "展开拒绝原因")
            Button("关闭") { model.clearScanSummary() }
                .font(.caption)
                .accessibilityLabel("关闭扫描摘要")
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 6)
        .background(Color.accentColor.opacity(0.08))
    }

    /// 选中操作栏（08309：多选删除/重排——仅 waiting 可用）。
    private var selectionActionBar: some View {
        return HStack(spacing: 8) {
            Button {
                showDeleteConfirmation = true
            } label: {
                Label("删除 (\(removableSelectionCount))", systemImage: "trash")
            }
            .disabled(removableSelectionCount == 0)
            .help("删除选中的等待任务")
            .accessibilityLabel("删除选中任务")

            Button {
                moveSelection(.up)
            } label: {
                Label("上移", systemImage: "arrow.up")
            }
            .disabled(!canMoveSelection(.up))
            .help("上移选中的等待任务")

            Button {
                moveSelection(.down)
            } label: {
                Label("下移", systemImage: "arrow.down")
            }
            .disabled(!canMoveSelection(.down))
            .help("下移选中的等待任务")

            Spacer()
            Text("选中 \(selection.count) 个")
                .font(.caption)
                .foregroundStyle(.secondary)
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 6)
    }

    private func canMoveSelection(_ direction: TaskCenterInteraction.MoveDirection) -> Bool {
        guard selection.count == 1, let id = selection.first,
              let index = model.state.tasks.firstIndex(where: { $0.id == id }) else { return false }
        let waitingIndices = model.state.tasks.indices.filter { model.state.tasks[$0].status == .waiting }
        guard let position = waitingIndices.firstIndex(of: index) else { return false }
        switch direction {
        case .up: return position > 0
        case .down: return position < waitingIndices.count - 1
        }
    }

    private func moveSelection(_ direction: TaskCenterInteraction.MoveDirection) {
        guard let id = selection.first else { return }
        var tasks = model.state.tasks
        TaskCenterInteraction.moveTask(id: id, in: &tasks, direction: direction)
        // reorder 参数 = 全部 waiting 任务的新顺序（槽位保留由 reorder 处理）。
        let waitingIDs = tasks.filter { $0.status == .waiting }.map(\.id)
        _ = model.scheduler.reorder(waitingIDs)
    }

    /// 08511：Inspector 卡片页眉的单任务移动（不依赖 selection）。
    private func moveSingleTask(_ direction: TaskCenterInteraction.MoveDirection, id: UUID) {
        var tasks = model.state.tasks
        TaskCenterInteraction.moveTask(id: id, in: &tasks, direction: direction)
        let waitingIDs = tasks.filter { $0.status == .waiting }.map(\.id)
        _ = model.scheduler.reorder(waitingIDs)
    }

    private func performDeleteSelection() {
        let removable = TaskCenterInteraction.removableTaskIDs(from: selectedTasks)
        for id in removable {
            _ = model.remove(id)
        }
        selection.removeAll()
    }

    // MARK: - Finder 定位（08309）

    /// 在 Finder 中定位源文件；不存在 → 错误提示（不静默）。
    private func revealSource(_ task: BatchTask) {
        if let url = TaskCenterInteraction.revealCandidate(for: task.sourceURL) {
            NSWorkspace.shared.activateFileViewerSelecting([url])
        } else {
            revealErrorURL = task.sourceURL
            showFinderError = true
        }
    }

    /// 在 Finder 中定位输出字幕；不存在 → 错误提示。
    private func revealOutput(_ task: BatchTask) {
        guard let output = task.outputURL else { return }
        if let url = TaskCenterInteraction.revealCandidate(for: output) {
            NSWorkspace.shared.activateFileViewerSelecting([url])
        } else {
            revealErrorURL = output
            showFinderError = true
        }
    }

    // MARK: - 导入（文件/文件夹/drop 同一 scanner）

    /// 先写入 kind，下一拍再 present，避免 fileImporter 用到旧的 allowedContentTypes。
    private func beginImport(_ kind: TaskCenterImportKind) {
        importKind = kind
        DispatchQueue.main.async {
            isImporting = true
        }
    }

    private func importDroppedProviders(_ providers: [NSItemProvider]) -> Bool {
        var urls: [URL] = []
        let lock = NSLock()
        let group = DispatchGroup()
        for provider in providers {
            group.enter()
            provider.loadItem(forTypeIdentifier: UTType.fileURL.identifier, options: nil) { item, _ in
                if let data = item as? Data, let url = URL(dataRepresentation: data, relativeTo: nil) {
                    lock.lock(); urls.append(url); lock.unlock()
                }
                group.leave()
            }
        }
        group.notify(queue: .main) {
            model.importInputs(urls)
        }
        return true
    }

    // MARK: - 冲突消息

    private var conflictMessage: String {
        model.outputConflicts.map { $0.outputURL.lastPathComponent }.joined(separator: "\n")
    }

    // MARK: - 空态（08511：虚线 drop zone）

    private var emptyCanvasDropZone: some View {
        let targeted = isDropTargeted || fixtureForceDropHighlight
        return VStack(spacing: 12) {
            Image(systemName: "rectangle.stack.badge.plus")
                .font(.system(size: 44))
                .foregroundStyle(.secondary)
            Text("将视频拖到这里")
                .font(.title3.weight(.medium))
            Text("拖入视频或文件夹，或使用工具栏添加")
                .font(.caption)
                .foregroundStyle(.secondary)
            Text("视频在本机处理，不会上传。")
                .font(.caption)
                .foregroundStyle(.secondary)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .padding(28)
        .background(targeted ? Color.accentColor.opacity(0.12) : Color(nsColor: .windowBackgroundColor))
        .overlay(
            RoundedRectangle(cornerRadius: 12)
                .strokeBorder(
                    targeted ? Color.accentColor : Color.secondary.opacity(0.55),
                    style: StrokeStyle(lineWidth: targeted ? 3 : 2, dash: [8, 4])
                )
        )
        .padding(16)
        .animation(.easeInOut(duration: 0.15), value: targeted)
    }

    /// 08511：DEBUG fixture——SUBLIFT_EVIDENCE_TASKCENTER_DROP=1 强制 drop 高亮态。
    private var fixtureForceDropHighlight: Bool {
        #if DEBUG
        return ProcessInfo.processInfo.environment["SUBLIFT_EVIDENCE_TASKCENTER_DROP"] == "1"
        #else
        return false
        #endif
    }
}

extension BatchInputRejectionReason {
    var localizedDescription: String {
        switch self {
        case .unsupportedFormat: "不支持的格式"
        case .mkvRequiresFfmpeg: "MKV 需要 ffmpeg"
        case .unreadable: "无法读取"
        case .duplicate: "重复文件"
        case .emptyDirectory: "空目录"
        }
    }
}
