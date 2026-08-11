import XCTest
@testable import SubLiftMac

/// 10206：Region 编辑视觉语义与可见性纯逻辑测试。
final class RegionEditingTests: XCTestCase {

    // MARK: - RegionBoxVisual 状态派生（不依赖颜色）

    func testUnselectedUnhoveredUsesSecondarySemantics() {
        let style = RegionBoxVisual.style(isSelected: false, isHovered: false, isMerged: false)
        XCTAssertFalse(style.strokeIsAccent)
        XCTAssertFalse(style.showsSelectionMark)
        XCTAssertLessThan(style.fillAlpha, 0.2)
    }

    func testHoverUsesAccentOutline() {
        let style = RegionBoxVisual.style(isSelected: false, isHovered: true, isMerged: false)
        XCTAssertTrue(style.strokeIsAccent, "hover 必须用 Accent outline 表达，不能只靠颜色")
        XCTAssertFalse(style.showsSelectionMark)
    }

    func testSelectedUsesAccentWithSelectionMark() {
        let style = RegionBoxVisual.style(isSelected: true, isHovered: false, isMerged: false)
        XCTAssertTrue(style.strokeIsAccent)
        XCTAssertTrue(style.showsSelectionMark, "选中必须有文字/控件标记（复选图标），不依赖颜色")
        XCTAssertGreaterThan(style.fillAlpha, 0.2)
    }

    func testMergedRegionUsesDistinctAccent() {
        let style = RegionBoxVisual.style(isSelected: false, isHovered: false, isMerged: true)
        XCTAssertTrue(style.strokeIsAccent)
        // merged 比普通候选更明确（更粗描边）。
        XCTAssertGreaterThan(style.lineWidth, 1.5)
    }

    func testStatesAreDistinguishableWithoutColor() {
        // 四种状态必须至少在一个非颜色维度（标记/粗细/透明度）上可区分。
        let states = [
            RegionBoxVisual.style(isSelected: false, isHovered: false, isMerged: false),
            RegionBoxVisual.style(isSelected: false, isHovered: true, isMerged: false),
            RegionBoxVisual.style(isSelected: true, isHovered: false, isMerged: false),
            RegionBoxVisual.style(isSelected: false, isHovered: false, isMerged: true),
        ]
        for i in 0..<states.count {
            for j in (i + 1)..<states.count {
                XCTAssertNotEqual(states[i], states[j], "状态 \(i) 与 \(j) 必须在非颜色维度上可区分")
            }
        }
    }

    // MARK: - Overlay 可见性（候选框仅 regionEditing）

    func testCandidateBoxesVisibleOnlyInRegionEditing() {
        XCTAssertTrue(RegionOverlayVisibility.showsCandidates(isRegionEditing: true))
        XCTAssertFalse(RegionOverlayVisibility.showsCandidates(isRegionEditing: false))
    }

    // MARK: - 高级几何格式化（真实字段）

    func testGeometryTextUsesRealCandidateFields() {
        let candidate = TextBoxCandidate(
            id: 3,
            pixelRect: CGRect(x: 120, y: 240, width: 480, height: 60),
            textPreview: "你好",
            confidence: 0.87,
            colorIndex: 0
        )
        let text = RegionInspectorPresentation.geometryText(for: candidate)
        XCTAssertTrue(text.contains("X 120"))
        XCTAssertTrue(text.contains("Y 240"))
        XCTAssertTrue(text.contains("480"))
        XCTAssertTrue(text.contains("60"))
        XCTAssertTrue(text.contains("87%"))
        XCTAssertFalse(text.contains("#3"), "高级几何不显示工程编号")
    }

    func testMergedSummaryUsesRealRegion() {
        let merged = CGRect(x: 0, y: 300, width: 1920, height: 80)
        let summary = RegionInspectorPresentation.mergedSummaryText(merged)
        XCTAssertTrue(summary.contains("300–380"), "应包含真实 Y 范围: \(summary)")
        XCTAssertTrue(summary.contains("全宽"))
    }
}
