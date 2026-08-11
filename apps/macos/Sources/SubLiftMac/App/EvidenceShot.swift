#if DEBUG
import AppKit

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
            guard let window = NSApp.keyWindow ?? NSApp.windows.first(where: { $0.isVisible }) else { return }
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

    /// 若设置了 SUBLIFT_EVIDENCE_EXTRACT=1，启动后以 Mock 引擎提取（Review 截图 fixture）。
    @MainActor
    static func extractFixtureIfRequested(workspace: WorkspaceModel) {
        if ProcessInfo.processInfo.environment["SUBLIFT_EVIDENCE_EXTRACT"] == "1" {
            // 等 metadata 加载完成（state → ready）后再开始提取；mock 引擎真实完成 → review。
            DispatchQueue.main.asyncAfter(deadline: .now() + 3.0) {
                workspace.startExtraction(engine: .mock, quality: .fast)
            }
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
