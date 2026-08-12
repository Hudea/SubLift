import XCTest
@testable import SubLiftMac

/// 10412：键盘路径目录（A01）——快捷键无重复、关键动作存在。
final class KeyboardShortcutCatalogTests: XCTestCase {

    func testNoDuplicateShortcuts() {
        let keys = KeyboardShortcutCatalog.entries.map { "\($0.modifiers)+\($0.key)" }
        XCTAssertEqual(Set(keys).count, keys.count, "快捷键不得重复注册")
    }

    func testCoreActionsRegistered() {
        let actions = KeyboardShortcutCatalog.entries.map(\.action)
        for required in ["打开视频", "导出 SRT", "拆分字幕", "与下一条合并", "播放/暂停", "编辑字幕区域", "显示 Inspector"] {
            XCTAssertTrue(actions.contains(required), "缺少快捷键动作：\(required)")
        }
    }

    func testEscapeExitsRegionEditingRegistered() {
        XCTAssertTrue(KeyboardShortcutCatalog.entries.contains { $0.key == "esc" && $0.action.contains("区域编辑") })
    }

    func testPlayPauseUsesBareSpace() {
        let play = KeyboardShortcutCatalog.entries.first { $0.action == "播放/暂停" }
        XCTAssertEqual(play?.key, " ")
        XCTAssertEqual(play?.modifiers, "")
    }
}

/// 10412：响应式布局回归（960×600 合同与窄窗口无溢出）。
final class ResponsiveLayoutTests: XCTestCase {

    func testStandardWidthsKeepContractRatios() {
        for width in [960.0, 1280.0, 1440.0] {
            let split = WorkspaceLayout.splitWidths(containerWidth: width)
            let ratio = split.video / (split.video + split.transcript)
            XCTAssertGreaterThanOrEqual(ratio, 0.60, "宽度 \(width) 下 video 比例过低")
            XCTAssertLessThanOrEqual(ratio, 0.65, "宽度 \(width) 下 video 比例过高")
            XCTAssertGreaterThanOrEqual(split.transcript, WorkspaceLayout.transcriptMinWidth)
        }
    }

    func testNarrowWindowWithInspectorDoesNotOverflow() {
        // 960 - 300（Inspector）= 660 可用：Transcript 保底 320，Video 让位，总宽不溢出。
        let split = WorkspaceLayout.splitWidths(containerWidth: 960, inspectorWidth: 300)
        let total = split.video + split.transcript
        XCTAssertLessThanOrEqual(total, 660 + 0.01)
        XCTAssertGreaterThanOrEqual(split.transcript, WorkspaceLayout.transcriptMinWidth)
        XCTAssertGreaterThan(split.video, 0)
    }

    func testClampedDragKeepsTranscriptAtLeastMinusHairline() {
        // 组合路径：Inspector 打开 + 用户把 divider 拖到最右——Transcript 保底 320（容许 1pt 分隔线）。
        let left = WorkspaceLayout.clampedLeftWidth(
            proposed: 900,
            containerWidth: 660,
            leftMin: 400,
            rightMin: 320
        )
        let transcript = 660 - left - 1
        XCTAssertGreaterThanOrEqual(transcript, 319, "Transcript 不得低于 320−1pt 分隔线：\(transcript)")
    }
}

/// 10414：实际 WorkspaceCommands 使用的 Esc 路由，而非只检查快捷键目录字符串。
@MainActor
final class WorkspaceCommandRoutingTests: XCTestCase {

    func testEscapeRouteExitsRegionEditing() {
        let model = makeReadyModel()
        model.enterRegionEditing()
        XCTAssertEqual(model.state, .regionEditing)

        XCTAssertTrue(WorkspaceCommandRouter.performEscape(on: model))
        XCTAssertEqual(model.state, .ready)
    }

    func testEscapeRouteDoesNotConsumeEscapeOutsideRegionEditing() {
        let model = makeReadyModel()

        XCTAssertFalse(WorkspaceCommandRouter.performEscape(on: model))
        XCTAssertEqual(model.state, .ready)
    }

    func testEscapeInterceptionRequiresBothPhysicalEscapeAndRegionEditing() {
        XCTAssertTrue(
            WorkspaceCommandRouter.shouldInterceptEscape(
                keyCode: 53,
                workspaceState: .regionEditing
            )
        )
        XCTAssertFalse(
            WorkspaceCommandRouter.shouldInterceptEscape(
                keyCode: 53,
                workspaceState: .ready
            ),
            "非区域编辑状态不得吞掉 Esc"
        )
        XCTAssertFalse(
            WorkspaceCommandRouter.shouldInterceptEscape(
                keyCode: 49,
                workspaceState: .regionEditing
            ),
            "Space 等其他 keyDown 必须原样传递"
        )
    }

    func testLocalMonitorInstallationAndRemovalAreIdempotent() {
        var installCount = 0
        var removeCount = 0
        let token = NSObject()
        let monitor = WorkspaceEscapeKeyMonitor(
            installer: { _ in
                installCount += 1
                return token
            },
            remover: { removed in
                XCTAssertTrue(removed as AnyObject === token)
                removeCount += 1
            }
        )
        let model = makeReadyModel()

        monitor.installIfNeeded(workspace: model)
        monitor.installIfNeeded(workspace: model)
        XCTAssertTrue(monitor.isInstalled)
        XCTAssertEqual(installCount, 1, "同一视图生命周期内不得重复安装 monitor")

        monitor.uninstall()
        monitor.uninstall()
        XCTAssertFalse(monitor.isInstalled)
        XCTAssertEqual(removeCount, 1, "拆除必须幂等，避免重复 remove 或泄漏")
    }

    func testLocalMonitorDeinitRemovesInstalledTokenAsSafetyNet() {
        var removeCount = 0
        weak var weakMonitor: WorkspaceEscapeKeyMonitor?
        do {
            let monitor = WorkspaceEscapeKeyMonitor(
                installer: { _ in NSObject() },
                remover: { _ in removeCount += 1 }
            )
            weakMonitor = monitor
            monitor.installIfNeeded(workspace: makeReadyModel())
            XCTAssertTrue(monitor.isInstalled)
        }

        XCTAssertNil(weakMonitor)
        XCTAssertEqual(removeCount, 1, "异常视图销毁路径也不得遗留 AppKit local monitor")
    }

    private func makeReadyModel() -> WorkspaceModel {
        let model = WorkspaceModel(metadataRequest: { url in
            VideoMetadata(
                fileName: url.lastPathComponent,
                fileSize: 0,
                width: 1920,
                height: 1080,
                durationMs: 1_000,
                codec: "h264"
            )
        })
        let url = URL(fileURLWithPath: "/tmp/command-route.mp4")
        XCTAssertTrue(model.openVideo(url: url))
        model.handleMetadataLoaded(
            VideoMetadata(
                fileName: url.lastPathComponent,
                fileSize: 0,
                width: 1920,
                height: 1080,
                durationMs: 1_000,
                codec: "h264"
            ),
            sessionToken: model.sessionToken
        )
        return model
    }
}

/// 10414：关键状态动作在 Toolbar 中强制使用 title + icon，而非仅依赖系统样式选择。
final class WorkspaceToolbarPresentationTests: XCTestCase {

    func testStateActionsUseVisibleTitles() throws {
        let sourceURL = repositoryRoot()
            .appendingPathComponent("apps/macos/Sources/SubLiftMac/UI/Workspace/WorkspaceRootView.swift")
        let source = try String(contentsOf: sourceURL, encoding: .utf8)

        XCTAssertTrue(source.contains("完成区域编辑"), "Toolbar 应包含区域编辑状态标题")
        XCTAssertTrue(source.contains("Label(\"提取字幕\""), "Toolbar 应包含提取字幕标题")
        XCTAssertTrue(source.contains("Label(\"停止\""), "Toolbar 应包含停止标题")
        XCTAssertGreaterThanOrEqual(
            source.components(separatedBy: ".labelStyle(.titleAndIcon)").count - 1,
            3,
            "区域编辑、提取、停止必须强制显示 title + icon"
        )
    }

    private func repositoryRoot() -> URL {
        var candidate = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
        while candidate.path != "/" {
            if FileManager.default.fileExists(
                atPath: candidate.appendingPathComponent("apps/macos/Package.swift").path
            ) {
                return candidate
            }
            candidate.deleteLastPathComponent()
        }
        XCTFail("找不到仓库根目录")
        return URL(fileURLWithPath: FileManager.default.currentDirectoryPath)
    }
}
