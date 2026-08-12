import XCTest
@testable import SubLiftMac

/// 10209：Subtitle Inspector 展示纯逻辑（只显示真实字段）。
final class SubtitleInspectorPresentationTests: XCTestCase {

    func testRowsShowRealFields() {
        let entry = SubtitleEntry(startMs: 12_345, endMs: 23_456, text: "你好，世界", confidence: 0.93)
        let rows = SubtitleInspectorPresentation.rows(entry: entry, index: 4, total: 10) ?? []
        let labels = rows.map(\.label)
        XCTAssertTrue(labels.contains("索引"))
        XCTAssertTrue(labels.contains("时间"))
        XCTAssertTrue(labels.contains("文本"))

        let indexRow = rows.first { $0.label == "索引" }
        XCTAssertEqual(indexRow?.value, "4 / 10")
        let timeRow = rows.first { $0.label == "时间" }
        XCTAssertEqual(timeRow?.value, "00:00:12.345 → 00:00:23.456")
        let textRow = rows.first { $0.label == "文本" }
        XCTAssertEqual(textRow?.value, "你好，世界")
    }

    func testLowConfidenceWarningOnlyWhenBelowThreshold() {
        let entry = SubtitleEntry(startMs: 0, endMs: 1000, text: "x", confidence: 0.5)
        let rows = SubtitleInspectorPresentation.rows(entry: entry, index: 1, total: 2) ?? []
        XCTAssertTrue(rows.contains { $0.label == "警告" && $0.value.contains("低置信") })
    }

    func testNoFakeFields() {
        // 不得出现不存在的字段（帧号、来源、语言等）。
        let entry = SubtitleEntry(startMs: 0, endMs: 1000, text: "x", confidence: 0.9)
        let rows = SubtitleInspectorPresentation.rows(entry: entry, index: 1, total: 2) ?? []
        for row in rows {
            XCTAssertFalse(row.label.contains("帧"))
            XCTAssertFalse(row.label.contains("来源"))
            XCTAssertFalse(row.label.contains("语言"))
        }
    }

    func testNoSelectionShowsEmptyState() {
        XCTAssertNil(SubtitleInspectorPresentation.rows(entry: nil, index: nil, total: 0))
    }
}

/// 10209：导出权限（只在 Review；processing/无结果不可导出）。
final class ExportAvailabilityTests: XCTestCase {

    private func makeAvailability(state: WorkspaceState, hasFinalEntries: Bool) -> WorkspaceCommandAvailability {
        WorkspaceCommandAvailability.derive(
            state: state,
            hasFinalEntries: hasFinalEntries,
            totalEntries: hasFinalEntries ? 3 : 0,
            selectedIndex: hasFinalEntries ? 0 : nil
        )
    }

    func testExportOnlyInReview() {
        XCTAssertTrue(makeAvailability(state: .review, hasFinalEntries: true).canExport)
        XCTAssertFalse(makeAvailability(state: .processing, hasFinalEntries: true).canExport)
        XCTAssertFalse(makeAvailability(state: .finalizing, hasFinalEntries: true).canExport)
        XCTAssertFalse(makeAvailability(state: .starting, hasFinalEntries: true).canExport)
        XCTAssertFalse(makeAvailability(state: .ready, hasFinalEntries: true).canExport)
        XCTAssertFalse(makeAvailability(state: .cancelled, hasFinalEntries: false).canExport)
        XCTAssertFalse(makeAvailability(state: .failed, hasFinalEntries: false).canExport)
    }

    func testExportRequiresFinalResult() {
        XCTAssertFalse(makeAvailability(state: .review, hasFinalEntries: false).canExport)
    }
}

/// 10209：Review 状态编辑/拆分/合并可用性。
final class ReviewEditingAvailabilityTests: XCTestCase {

    func testReviewAllowsEditSplitMerge() {
        let availability = WorkspaceCommandAvailability.derive(
            state: .review,
            hasFinalEntries: true,
            totalEntries: 3,
            selectedIndex: 1
        )
        XCTAssertTrue(availability.canEdit)
        XCTAssertTrue(availability.canSplit)
        XCTAssertTrue(availability.canMerge)
    }

    func testProcessingDisablesMutations() {
        let availability = WorkspaceCommandAvailability.derive(
            state: .processing,
            hasFinalEntries: false,
            totalEntries: 0,
            selectedIndex: nil
        )
        XCTAssertFalse(availability.canEdit)
        XCTAssertFalse(availability.canSplit)
        XCTAssertFalse(availability.canMerge)
        XCTAssertFalse(availability.canExport)
    }
}
