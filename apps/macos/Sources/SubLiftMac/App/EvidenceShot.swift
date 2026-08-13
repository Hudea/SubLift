#if DEBUG
import AppKit
import SwiftUI

/// Phase 10 视觉验收辅助：将当前可见窗口渲染为 PNG（仅 DEBUG 构建）。
///
/// 通过环境变量触发，**生产行为不变**（release 构建不含本文件；无环境变量时零副作用）：
/// - `SUBLIFT_EVIDENCE_SHOT=<path>`：截图保存路径（app 内自渲染，不依赖系统录屏权限）。
/// - `SUBLIFT_EVIDENCE_DELAY=<seconds>`：截图延迟（默认 0；用于截取拖拽悬停等瞬时状态）。
/// - `SUBLIFT_EVIDENCE_DROP=1`：将 Welcome 拖拽激活视觉强制为 true（稳定截图 fixture）。
enum EvidenceShot {

    /// Welcome 拖拽激活视觉 fixture（SUBLIFT_EVIDENCE_DROP=1 时启用）。
    static let isDropTargetFixture: Bool = {
        ProcessInfo.processInfo.environment["SUBLIFT_EVIDENCE_DROP"] == "1"
    }()

    /// 若设置了 SUBLIFT_EVIDENCE_SHOT，则在（可选延迟后）渲染当前窗口并保存。
    static func scheduleIfRequested() {
        guard let path = ProcessInfo.processInfo.environment["SUBLIFT_EVIDENCE_SHOT"] else { return }
        let delay = Double(ProcessInfo.processInfo.environment["SUBLIFT_EVIDENCE_DELAY"] ?? "0") ?? 0
        DispatchQueue.main.asyncAfter(deadline: .now() + delay) {
            // keyWindow 优先；多窗口（Settings fixture）时取最后创建的可见窗口。
            guard let window = NSApp.keyWindow ?? NSApp.windows.last(where: { $0.isVisible }) else { return }
            save(window: window, to: path)
        }
    }

    /// 若设置了 SUBLIFT_EVIDENCE_OPEN=<视频路径>，启动后自动导入（ready 状态截图 fixture）。
    @MainActor
    static func autoOpenIfRequested(workspace: WorkspaceModel) {
        guard let path = ProcessInfo.processInfo.environment["SUBLIFT_EVIDENCE_OPEN"] else { return }
        _ = workspace.openVideo(url: URL(fileURLWithPath: path))
    }

    /// 若设置了 SUBLIFT_EVIDENCE_INSPECTOR=1，启动后打开 Inspector（V03 截图 fixture）。
    @MainActor
    static func inspectorFixtureIfRequested(workspace: WorkspaceModel) {
        if ProcessInfo.processInfo.environment["SUBLIFT_EVIDENCE_INSPECTOR"] == "1" {
            workspace.setInspectorPresented(true)
        }
    }

    /// 若设置了 SUBLIFT_EVIDENCE_REGION=1，启动后进入 Region Editing（V04 截图 fixture）。
    @MainActor
    static func regionFixtureIfRequested(workspace: WorkspaceModel) {
        if ProcessInfo.processInfo.environment["SUBLIFT_EVIDENCE_REGION"] == "1" {
            // 等 metadata 加载完成（state → ready）后再进入区域编辑。
            DispatchQueue.main.asyncAfter(deadline: .now() + 3.0) {
                workspace.enterRegionEditing()
            }
        }
    }

    /// 若设置了 SUBLIFT_EVIDENCE_EXTRACT=1，启动后提取（Review/Processing 截图 fixture）。
    /// 引擎由 SUBLIFT_EVIDENCE_ENGINE 指定（默认 mock；vision 用于捕捉真实 processing 状态）。
    @MainActor
    static func extractFixtureIfRequested(workspace: WorkspaceModel) {
        guard ProcessInfo.processInfo.environment["SUBLIFT_EVIDENCE_EXTRACT"] == "1" else { return }
        let engineName = ProcessInfo.processInfo.environment["SUBLIFT_EVIDENCE_ENGINE"] ?? "mock"
        let engine: OcrEngineName = switch engineName {
        case "vision": .vision
        case "paddle": .paddle
        default: .mock
        }
        // 等 metadata 加载完成（state → ready）后再开始提取。
        DispatchQueue.main.asyncAfter(deadline: .now() + 3.0) {
            workspace.startExtraction(engine: engine, quality: .fast)
        }
    }

    /// 若设置了 SUBLIFT_EVIDENCE_SELECT=1，提取完成后选中第一条字幕（Subtitle Inspector 截图 fixture）。
    @MainActor
    static func selectFixtureIfRequested(workspace: WorkspaceModel) {
        guard ProcessInfo.processInfo.environment["SUBLIFT_EVIDENCE_SELECT"] == "1" else { return }
        // 等 mock 提取完成（entries 就绪）后选中第一条。
        DispatchQueue.main.asyncAfter(deadline: .now() + 8.0) {
            if let first = workspace.editor.entries.first {
                workspace.selectSubtitle(id: first.id)
            }
        }
    }

    /// 若设置了 SUBLIFT_EVIDENCE_COMPACT=1，将主窗口内容缩放到 960×600（V09b 紧凑截图 fixture）。
    @MainActor
    static func compactFixtureIfRequested() {
        guard ProcessInfo.processInfo.environment["SUBLIFT_EVIDENCE_COMPACT"] == "1" else { return }
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) {
            NSApp.windows.first(where: { $0.isVisible })?.setContentSize(NSSize(width: 960, height: 600))
        }
    }

    /// 若设置了 SUBLIFT_EVIDENCE_PENDING=1，提取完成后修改采样偏好（显示"配置已更改"待生效状态截图 fixture）。
    /// 仅 DEBUG 截图用：演示 final 配置与偏好不同的 UI 状态，不冒充真实 OCR 结果。
    @MainActor
    static func pendingFixtureIfRequested() {
        guard ProcessInfo.processInfo.environment["SUBLIFT_EVIDENCE_PENDING"] == "1" else { return }
        // 等 mock 提取完成（final 配置已由启动时快照确定）后改写采样偏好。
        DispatchQueue.main.asyncAfter(deadline: .now() + 8.0) {
            UserDefaults.standard.set(SamplingQuality.fine.rawValue, forKey: "sampling_quality")
        }
    }

    /// 若设置了 SUBLIFT_EVIDENCE_ENTRIES=1，导入后注入测试字幕条目（Timeline/Transcript 截图 fixture）。
    /// 注意：仅用于视觉验收截图；生产路径（提取/Review）不经过此 fixture。
    @MainActor
    static func entriesFixtureIfRequested(workspace: WorkspaceModel) {
        guard ProcessInfo.processInfo.environment["SUBLIFT_EVIDENCE_ENTRIES"] == "1" else { return }
        DispatchQueue.main.asyncAfter(deadline: .now() + 4.0) {
            let data = [
                SubtitleEntryData(startMs: 500, endMs: 3_000, text: "Hello SubLift", confidence: 0.95),
                SubtitleEntryData(startMs: 4_000, endMs: 7_000, text: "第二行字幕内容", confidence: 0.88),
                SubtitleEntryData(startMs: 9_000, endMs: 14_000, text: "A short line", confidence: 0.62),
                SubtitleEntryData(startMs: 20_000, endMs: 22_000, text: "Last", confidence: 0.4),
            ]
            workspace.editor.load(data)
            workspace.selectSubtitle(id: workspace.editor.entries.first?.id)
        }
    }

    /// 若设置了 SUBLIFT_EVIDENCE_SETTINGS=1，创建承载 SettingsView 的窗口（V07 截图 fixture）。
    /// 仅 DEBUG 截图用：真实渲染 SettingsView，截图后由进程退出清理。
    @MainActor
    static func settingsFixtureIfRequested() {
        guard ProcessInfo.processInfo.environment["SUBLIFT_EVIDENCE_SETTINGS"] == "1" else { return }
        DispatchQueue.main.asyncAfter(deadline: .now() + 2.0) {
            let hosting = NSHostingController(rootView: SettingsView())
            let window = NSWindow(contentViewController: hosting)
            window.setContentSize(NSSize(width: 500, height: 340))
            window.title = "Settings"
            window.styleMask = [.titled, .closable]
            window.isReleasedWhenClosed = false
            NSApp.activate(ignoringOtherApps: true)
            window.makeKeyAndOrderFront(nil)
        }
    }

    static func save(window: NSWindow, to path: String) {
        guard let view = window.contentView else { return }
        guard let rep = view.bitmapImageRepForCachingDisplay(in: view.bounds) else { return }
        view.cacheDisplay(in: view.bounds, to: rep)
        guard let data = rep.representation(using: .png, properties: [:]) else { return }
        try? data.write(to: URL(fileURLWithPath: path))
    }
}
#endif

// MARK: - 08308 Task Center fixture（DEBUG-only，确定性状态截图；不冒充真实 OCR）

#if DEBUG
extension EvidenceShot {
    /// SUBLIFT_EVIDENCE_TASKCENTER=EMPTY|WAITING|RUNNING|PAUSED|MIXED|RESTORED|ERROR
    /// 注入确定性队列状态并打开 Task Center 窗口（截图用；不冒充真实 OCR）。
    @MainActor
    static func taskCenterFixtureIfRequested(model: BatchQueueModel) {
        guard let raw = ProcessInfo.processInfo.environment["SUBLIFT_EVIDENCE_TASKCENTER"] else { return }
        model.installFixture(makeFixtureState(raw))
        // B02 组合：SCAN=1 → 真实临时文件导入 → 扫描摘要横幅（接受/拒绝计数）。
        if ProcessInfo.processInfo.environment["SUBLIFT_EVIDENCE_SCAN"] == "1" {
            scanFixtureIfRequested(model: model)
        }
        openTaskCenterWindow(model: model)
        scheduleTaskCenterShotIfRequested()
    }

    /// B02 fixture：真实临时文件（2 视频 + 1 不支持扩展）导入 → lastScanSummary 横幅。
    @MainActor
    private static func scanFixtureIfRequested(model: BatchQueueModel) {
        let dir = FileManager.default.temporaryDirectory
            .appendingPathComponent("sublift-b02-fixture", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let files = ["interview_clip.mp4", "lecture_slides.mov", "notes.txt"]
        for name in files {
            let url = dir.appendingPathComponent(name)
            try? Data("fixture video bytes".utf8).write(to: url)
        }
        model.importInputs([dir])
    }

    @MainActor
    private static func openTaskCenterWindow(model: BatchQueueModel) {
        // 隐藏主窗口（fixture 专属）：避免主窗口抢 key 使 Task Center 按钮呈非激活灰色。
        for window in NSApp.windows where window.identifier?.rawValue != "task-center" {
            window.orderOut(nil)
        }
        if let window = NSApp.windows.first(where: { $0.identifier?.rawValue == "task-center" }) {
            window.makeKeyAndOrderFront(nil)
        } else {
            let hosting = NSHostingController(rootView: TaskCenterView(model: model))
            let window = NSWindow(contentViewController: hosting)
            window.identifier = NSUserInterfaceItemIdentifier("task-center")
            window.title = "任务中心"
            // B09 fixture：窗口级 Dark（不切系统外观）。
            if ProcessInfo.processInfo.environment["SUBLIFT_EVIDENCE_DARK"] == "1" {
                window.appearance = NSAppearance(named: .darkAqua)
            }
            // COMPACT=1 → 960×600（紧凑截图 fixture）。
            if ProcessInfo.processInfo.environment["SUBLIFT_EVIDENCE_COMPACT"] == "1" {
                window.setContentSize(NSSize(width: 960, height: 600))
            } else {
                window.setContentSize(NSSize(width: 1280, height: 800))
            }
            window.makeKeyAndOrderFront(nil)
        }
        NSApp.activate(ignoringOtherApps: true)
    }

    @MainActor
    private static func scheduleTaskCenterShotIfRequested() {
        guard let shotPath = ProcessInfo.processInfo.environment["SUBLIFT_EVIDENCE_SHOT"] else { return }
        let delay = Double(ProcessInfo.processInfo.environment["SUBLIFT_EVIDENCE_DELAY"] ?? "6") ?? 6
        DispatchQueue.main.asyncAfter(deadline: .now() + delay) {
            guard let window = NSApp.windows.first(where: { $0.identifier?.rawValue == "task-center" }) else {
                print("[EvidenceShot] task-center window not found")
                return
            }
            // 强制激活目标窗口（离屏渲染对 inactive 窗口可能输出全黑）。
            window.makeKeyAndOrderFront(nil)
            NSApp.activate(ignoringOtherApps: true)
            // 强制重建窗口内容（SwiftUI hosting 离屏缓存失效场景）。
            window.orderOut(nil)
            window.orderFront(nil)
            window.makeKeyAndOrderFront(nil)
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
                if let view = window.contentView {
                let size = view.bounds.size
                // 强制布局 + 显示（cacheDisplay 对未完成首帧/后台窗口可能得到全黑）。
                view.layoutSubtreeIfNeeded()
                view.displayIfNeeded()
                // 方案一：离屏位图（与 10415 一致）。
                var saved = false
                if let bitmap = view.bitmapImageRepForCachingDisplay(in: view.bounds) {
                    view.cacheDisplay(in: view.bounds, to: bitmap)
                    if let png = bitmap.representation(using: .png, properties: [:]) {
                        // 空态窗口实测 cacheDisplay 可能得到全黑——采样中心像素检测。
                        let isBlack = isMostlyBlack(bitmap, size: size)
                        if !isBlack {
                            try? png.write(to: URL(fileURLWithPath: shotPath))
                            saved = true
                        }
                    }
                }
                // 方案二：cacheDisplay 全黑时用 PDF 矢量渲染兜底。
                if !saved {
                    let pdf = view.dataWithPDF(inside: view.bounds)
                    if let image = NSImage(data: pdf),
                       let tiff = image.tiffRepresentation,
                       let rep = NSBitmapImageRep(data: tiff),
                       let png = rep.representation(using: .png, properties: [:]) {
                        try? png.write(to: URL(fileURLWithPath: shotPath))
                        saved = true
                    }
                }
                // 方案三：屏幕级捕获（CGWindowListCreateImage，需屏幕录制权限；SwiftUI 空态
                // 离屏渲染（cacheDisplay/PDF）实测全黑时使用）。
                if !saved {
                    let windowID = CGWindowID(window.windowNumber)
                    if let image = CGWindowListCreateImage(
                        .null,
                        [.optionIncludingWindow],
                        windowID,
                        [.boundsIgnoreFraming, .bestResolution]
                    ) {
                        let rep = NSBitmapImageRep(cgImage: image)
                        if let png = rep.representation(using: .png, properties: [:]) {
                            try? png.write(to: URL(fileURLWithPath: shotPath))
                            saved = true
                        }
                    }
                }
                print("[EvidenceShot] saved \(shotPath) (\(Int(size.width))x\(Int(size.height)) points, saved=\(saved))")
                }
            }
        }
    }

    /// 采样位图中心区域判断是否全黑（空态窗口 cacheDisplay 黑屏检测）。
    private static func isMostlyBlack(_ bitmap: NSBitmapImageRep, size: CGSize) -> Bool {
        let pixelWidth = bitmap.pixelsWide
        let pixelHeight = bitmap.pixelsHigh
        guard pixelWidth > 0, pixelHeight > 0 else { return true }
        let cx = pixelWidth / 2
        let cy = pixelHeight / 2
        let samples = [(cx, cy), (cx - 50, cy), (cx + 50, cy), (cx, cy - 50), (cx, cy + 50)]
        var dark = 0
        for (x, y) in samples where x >= 0 && x < pixelWidth && y >= 0 && y < pixelHeight {
            if let c = bitmap.colorAt(x: x, y: y)?.usingColorSpace(.deviceRGB) {
                if c.redComponent < 0.05, c.greenComponent < 0.05, c.blueComponent < 0.05 {
                    dark += 1
                }
            }
        }
        return dark >= 4
    }

    @MainActor
    static func makeFixtureState(_ raw: String) -> BatchQueueState {
        let now = Date()
        func task(_ name: String, status: BatchTaskStatus, progress: Double? = nil, output: String? = nil, importRoot: URL? = nil) -> BatchTask {
            var t = BatchTask.make(
                sourceURL: URL(fileURLWithPath: "/tmp/sample/\(name)"),
                engine: .vision,
                quality: .fast,
                developerMode: false,
                createdAt: now,
                importRootURL: importRoot
            )
            switch status {
            case .preparing: _ = t.transition(to: .preparing)
            case .extracting:
                _ = t.transition(to: .preparing)
                _ = t.transition(to: .extracting)
            case .exporting:
                _ = t.transition(to: .preparing)
                _ = t.transition(to: .extracting)
                _ = t.transition(to: .exporting)
            case .completed:
                _ = t.transition(to: .preparing)
                _ = t.transition(to: .extracting)
                _ = t.transition(to: .exporting)
                _ = t.transition(to: .completed)
            case .failed:
                // 先记录错误（活动态守卫），再进入终态。
                _ = t.transition(to: .preparing)
                t.recordFailure("提取失败: 无法启动 Worker（未找到可执行文件）")
                _ = t.transition(to: .failed)
            case .cancelled: _ = t.transition(to: .cancelled)
            case .interrupted:
                _ = t.transition(to: .preparing)
                _ = t.transition(to: .interrupted)
            case .skipped: _ = t.transition(to: .skipped)
            case .waiting: break
            }
            if let progress { t.setProgress(progress) }
            if let output { t.outputURL = URL(fileURLWithPath: output) }
            if status == .completed, let output {
                t.recordResult(BatchTaskResult(entryCount: 42, outputURL: URL(fileURLWithPath: output), runtimeIdentity: "cpp"))
            }
            return t
        }

        var queue = BatchQueueState.empty
        switch raw {
        case "EMPTY":
            queue.status = .idle
        case "WAITING":
            let root = URL(fileURLWithPath: "/tmp/sample/Zootopia")
            queue.tasks = [task("interview_01.mp4", status: .waiting, importRoot: root),
                           task("lecture_part2.mov", status: .waiting, importRoot: root),
                           task("meeting_recording.mkv", status: .waiting, importRoot: root)]
            queue.status = .idle
        case "RUNNING":
            queue.tasks = [task("interview_01.mp4", status: .extracting, progress: 0.42, output: "/tmp/sample/interview_01.srt"),
                           task("lecture_part2.mov", status: .waiting),
                           task("meeting_recording.mkv", status: .waiting)]
            queue.status = .running
            queue.runningTaskID = queue.tasks[0].id
        case "PAUSED":
            queue.tasks = [task("interview_01.mp4", status: .completed, output: "/tmp/sample/interview_01.srt"),
                           task("lecture_part2.mov", status: .waiting),
                           task("meeting_recording.mkv", status: .waiting)]
            queue.status = .paused
        case "MIXED":
            queue.tasks = [task("a.mp4", status: .completed, output: "/tmp/sample/a.srt"),
                           task("b.mov", status: .failed),
                           task("c.mp4", status: .skipped),
                           task("d.mp4", status: .waiting)]
            queue.status = .idle
        case "RESTORED":
            queue.tasks = [task("a.mp4", status: .interrupted),
                           task("b.mov", status: .waiting),
                           task("c.mp4", status: .completed, output: "/tmp/sample/c.srt")]
            queue.status = .paused
        case "ERROR":
            // 失败任务先记录错误（活动态）再进入 failed——否则 recordFailure 守卫拒绝。
            var failed = task("a.mp4", status: .waiting)
            _ = failed.transition(to: .preparing)
            failed.recordFailure("提取失败: 无法启动 Worker（未找到可执行文件）")
            _ = failed.transition(to: .failed)
            queue.tasks = [failed, task("b.mov", status: .waiting)]
            queue.status = .paused
        default:
            queue.status = .idle
        }
        return queue
    }
}
#endif // 08308 Task Center fixture（DEBUG-only）
