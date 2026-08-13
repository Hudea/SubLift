import XCTest
@testable import SubLiftMac

/// 10104：Workspace Split 布局纯逻辑测试。
///
/// 合同（docs/design_ui/interaction-state-spec.md §7）：
/// - Video 60–65% / Transcript 35–40%；
/// - 变窄时先回收 Inspector 预留位，再把 Transcript 缩至不低于约 320pt；
/// - 1280×800 与 960×600 均不得把主动作挤出。
final class WorkspaceLayoutTests: XCTestCase {

    func testWelcomeWindowIsCompactVersusWorkbench() {
        XCTAssertLessThan(WorkspaceLayout.welcomeWindowSize.width, WorkspaceLayout.workbenchWindowMinSize.width)
        XCTAssertLessThan(WorkspaceLayout.welcomeWindowSize.height, WorkspaceLayout.workbenchWindowMinSize.height)
        XCTAssertGreaterThanOrEqual(WorkspaceLayout.welcomeWindowSize.width, WorkspaceLayout.welcomeWindowMinSize.width)
        XCTAssertEqual(WorkspaceLayout.workbenchWindowSize, CGSize(width: 1280, height: 800))
        XCTAssertEqual(WorkspaceLayout.workbenchWindowMinSize, CGSize(width: 960, height: 600))
    }

    func testIdealWidth1280SplitsWithinContract() {
        let split = WorkspaceLayout.splitWidths(containerWidth: 1280)
        let ratio = split.video / split.total
        XCTAssertEqual(ratio, 0.62, accuracy: 0.001)
        XCTAssertGreaterThanOrEqual(ratio, 0.60)
        XCTAssertLessThanOrEqual(ratio, 0.65)
        XCTAssertGreaterThanOrEqual(split.transcript, WorkspaceLayout.transcriptMinWidth)
        XCTAssertEqual(split.video + split.transcript, 1280, accuracy: 0.001)
    }

    func testMinWidth960SplitsWithinContract() {
        let split = WorkspaceLayout.splitWidths(containerWidth: 960)
        let ratio = split.video / split.total
        XCTAssertGreaterThanOrEqual(ratio, 0.60)
        XCTAssertLessThanOrEqual(ratio, 0.65)
        XCTAssertGreaterThanOrEqual(split.transcript, WorkspaceLayout.transcriptMinWidth)
    }

    func testNarrowWidthKeepsTranscriptAboveMinimum() {
        // 极端窄窗口：Transcript 保底 320，不崩溃、不出现负宽度。
        let split = WorkspaceLayout.splitWidths(containerWidth: 700)
        XCTAssertGreaterThanOrEqual(split.transcript, WorkspaceLayout.transcriptMinWidth)
        XCTAssertGreaterThan(split.video, 0)
    }

    func testInspectorReservationIsReclaimedFirst() {
        // Inspector 打开时先从其预留位回收；Transcript 仍 ≥ 320。
        let withInspector = WorkspaceLayout.splitWidths(containerWidth: 960, inspectorWidth: 300)
        XCTAssertGreaterThanOrEqual(withInspector.transcript, WorkspaceLayout.transcriptMinWidth)
        XCTAssertLessThan(withInspector.video, WorkspaceLayout.splitWidths(containerWidth: 960).video)
        // Inspector 关闭时布局更宽，视频不被压缩。
        let withoutInspector = WorkspaceLayout.splitWidths(containerWidth: 960)
        XCTAssertGreaterThan(withoutInspector.video, withInspector.video)
    }

    func testWideContainerKeepsTranscriptProportional() {
        // 全屏/更宽时不把 Transcript 无上限拉宽：仍按比例受约束。
        let split = WorkspaceLayout.splitWidths(containerWidth: 1920)
        let ratio = split.transcript / split.total
        XCTAssertLessThanOrEqual(ratio, 0.40)
        XCTAssertGreaterThanOrEqual(ratio, 0.35)
    }

    func testSizesAreMonotonicInContainerWidth() {
        // 容器变宽时 video/transcript 单调不减（无跳变）。
        var previous = WorkspaceLayout.splitWidths(containerWidth: 640)
        for width in stride(from: 641, through: 1600, by: 50) {
            let split = WorkspaceLayout.splitWidths(containerWidth: CGFloat(width))
            XCTAssertGreaterThanOrEqual(split.video, previous.video)
            XCTAssertGreaterThanOrEqual(split.transcript, previous.transcript)
            previous = split
        }
    }

    func testInspectorWidthConstantIsInDesignRange() {
        // Inspector 预留位在 280–360pt 设计范围内（10105 使用）。
        XCTAssertGreaterThanOrEqual(WorkspaceLayout.inspectorReservedWidth, 280)
        XCTAssertLessThanOrEqual(WorkspaceLayout.inspectorReservedWidth, 360)
    }

    func testClampedLeftWidthKeepsBothSidesAboveMinimum() {
        // 初始：1280 容器，左 62%（793.6），右 38%（486.4）。
        let initial = WorkspaceLayout.splitWidths(containerWidth: 1280).video
        XCTAssertEqual(initial, 793.6, accuracy: 0.1)

        // 拖动 +300：受右栏最小宽约束（1280-320=960）。
        let draggedRight = WorkspaceLayout.clampedLeftWidth(
            proposed: initial + 300, containerWidth: 1280, leftMin: 400, rightMin: 320
        )
        XCTAssertEqual(draggedRight, 960, accuracy: 0.1)

        // 拖动 -500：受左栏最小宽约束（400）。
        let draggedLeft = WorkspaceLayout.clampedLeftWidth(
            proposed: initial - 500, containerWidth: 1280, leftMin: 400, rightMin: 320
        )
        XCTAssertEqual(draggedLeft, 400, accuracy: 0.1)

        // 窄容器：960，左栏最小 400、右栏最小 320 → 上限 640。
        let narrow = WorkspaceLayout.clampedLeftWidth(
            proposed: 900, containerWidth: 960, leftMin: 400, rightMin: 320
        )
        XCTAssertEqual(narrow, 640, accuracy: 0.1)
    }

    func testClampedLeftWidthWithInspectorReserved() {
        // Inspector 打开（300）时，Split 可用宽度 = 容器 - 300。
        let container = WorkspaceLayout.splitWidths(containerWidth: 1280, inspectorWidth: 300)
        XCTAssertEqual(container.total, 980, accuracy: 0.1)
        XCTAssertEqual(container.video, 607.6, accuracy: 0.1)

        // 拖动上限按可用宽度计算。
        let clamped = WorkspaceLayout.clampedLeftWidth(
            proposed: 800, containerWidth: 1280 - 300, leftMin: 400, rightMin: 320
        )
        XCTAssertEqual(clamped, 660, accuracy: 0.1)
    }

    func testClampedLeftWidthYieldsToTranscriptWhenSpaceTight() {
        // 960 窗口 + Inspector 300 → 可用 660 < 400+320：Video 让位保证 Transcript ≥320，
        // 不产生 HStack 溢出。
        let left = WorkspaceLayout.clampedLeftWidth(
            proposed: 607.6, containerWidth: 660, leftMin: 400, rightMin: 320
        )
        XCTAssertEqual(left, 340, accuracy: 0.1)
        XCTAssertGreaterThanOrEqual(660 - left - 1, 319)

        // 空间充足时正常 clamp（1200 容器）。
        let normal = WorkspaceLayout.clampedLeftWidth(
            proposed: 700, containerWidth: 1200, leftMin: 400, rightMin: 320
        )
        XCTAssertEqual(normal, 700, accuracy: 0.1)
        // 拖动到极限：右栏保底 320。
        let extreme = WorkspaceLayout.clampedLeftWidth(
            proposed: 9999, containerWidth: 1200, leftMin: 400, rightMin: 320
        )
        XCTAssertEqual(extreme, 880, accuracy: 0.1)
    }

    func testResolvedColumnsReclampCachedWidthWhenInspectorShrinksContainer() {
        // 先在 1280 宽布局中得到 793.6pt 的 Video；Inspector 打开后 Split 容器缩至 980。
        // 即使缓存仍是旧宽度，也必须重新 clamp，且 1pt divider 计入总宽，不能挤压 Transcript。
        let collapsed = WorkspaceLayout.resolvedColumns(
            proposedLeftWidth: 793.6,
            containerWidth: 980,
            dividerWidth: 1,
            leftMin: 400,
            rightMin: 320
        )

        XCTAssertEqual(collapsed.left, 659, accuracy: 0.1)
        XCTAssertEqual(collapsed.right, 320, accuracy: 0.1)
        XCTAssertEqual(collapsed.total, 980, accuracy: 0.1)
    }

    func testResolvedColumnsAcceptCurrentPreferenceAfterContainerExpands() {
        // 容器恢复后，当前宽度偏好落在新边界内时保持不变。
        let expanded = WorkspaceLayout.resolvedColumns(
            proposedLeftWidth: 793.6,
            containerWidth: 1280,
            dividerWidth: 1,
            leftMin: 400,
            rightMin: 320
        )

        XCTAssertEqual(expanded.left, 793.6, accuracy: 0.1)
        XCTAssertEqual(expanded.right, 485.4, accuracy: 0.1)
        XCTAssertEqual(expanded.total, 1280, accuracy: 0.1)
    }

    func testResolvedColumnsStayWithinContainerAcrossRepeatedDrags() {
        for proposed in [793.6, 1200, 200, 900, 659, 10_000] {
            let columns = WorkspaceLayout.resolvedColumns(
                proposedLeftWidth: proposed,
                containerWidth: 980,
                dividerWidth: 1,
                leftMin: 400,
                rightMin: 320
            )
            XCTAssertEqual(columns.total, 980, accuracy: 0.1)
            XCTAssertGreaterThanOrEqual(columns.right, 320)
            XCTAssertGreaterThanOrEqual(columns.left, 400)
        }
    }
}
