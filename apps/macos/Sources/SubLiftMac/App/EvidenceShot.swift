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
