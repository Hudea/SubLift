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
