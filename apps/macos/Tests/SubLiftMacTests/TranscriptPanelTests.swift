import XCTest
@testable import SubLiftMac

/// 10207：Transcript 搜索过滤投影（只影响显示，不改变 entries/导出）。
final class TranscriptSearchTests: XCTestCase {

    private func makeEntries() -> [SubtitleEntry] {
        [
            SubtitleEntry(startMs: 0, endMs: 1000, text: "Hello World", confidence: 0.9),
            SubtitleEntry(startMs: 1000, endMs: 2000, text: "第二行字幕", confidence: 0.7),
            SubtitleEntry(startMs: 2000, endMs: 3000, text: "Long text here", confidence: 0.4),
        ]
    }

    func testEmptyQueryMatchesAll() {
        let entries = makeEntries()
        XCTAssertEqual(TranscriptSearch.filteredEntries(entries, query: "").count, 3)
        XCTAssertEqual(TranscriptSearch.filteredEntries(entries, query: "  ").count, 3)
    }

    func testSubstringMatchCaseInsensitive() {
        let entries = makeEntries()
        let hit = TranscriptSearch.filteredEntries(entries, query: "hello")
        XCTAssertEqual(hit.count, 1)
        XCTAssertEqual(hit.first?.text, "Hello World")
    }

    func testCJKAndPartialMatch() {
        let entries = makeEntries()
        let hit = TranscriptSearch.filteredEntries(entries, query: "二行")
        XCTAssertEqual(hit.count, 1)
        XCTAssertEqual(hit.first?.text, "第二行字幕")
    }

    func testFilteringDoesNotMutateEntriesOrOrder() {
        let entries = makeEntries()
        let originalTexts = entries.map(\.text)
        _ = TranscriptSearch.filteredEntries(entries, query: "world")
        // 过滤是只读投影：原 entries 顺序与内容不变（导出不受影响）。
        XCTAssertEqual(entries.map(\.text), originalTexts)
    }

    func testNoResultsReturnsEmpty() {
        let entries = makeEntries()
        XCTAssertTrue(TranscriptSearch.filteredEntries(entries, query: "不存在的内容").isEmpty)
    }
}

/// 10207：Transcript 行展示纯逻辑。
final class TranscriptRowPresentationTests: XCTestCase {

    func testLowConfidenceWarningOnlyBelowThreshold() {
        XCTAssertTrue(TranscriptRowPresentation.needsLowConfidenceWarning(confidence: 0.59))
        XCTAssertFalse(TranscriptRowPresentation.needsLowConfidenceWarning(confidence: 0.6))
        XCTAssertFalse(TranscriptRowPresentation.needsLowConfidenceWarning(confidence: 0.95))
    }

    func testThresholdIsExplicitConstant() {
        // 显式 UI 常量（不散落 magic number），且在合理警告区间。
        XCTAssertEqual(TranscriptRowPresentation.lowConfidenceThreshold, 0.6, accuracy: 0.01)
    }

    func testTimeCodeTextUsesMonoFormat() {
        let entry = SubtitleEntry(startMs: 61_240, endMs: 74_920, text: "x", confidence: 0.9)
        let timeText = TranscriptRowPresentation.timeCodeText(for: entry)
        XCTAssertTrue(timeText.contains("→"))
        XCTAssertTrue(timeText.contains(TimeFormatter.formatMs(entry.startMs)))
        XCTAssertTrue(timeText.contains(TimeFormatter.formatMs(entry.endMs)))
    }
}

/// 10207：行级上下文命令可用性（边界 Merge 禁用、只读禁用）。
final class TranscriptRowAvailabilityTests: XCTestCase {

    private func makeEntries() -> [SubtitleEntry] {
        [
            SubtitleEntry(startMs: 0, endMs: 1000, text: "A", confidence: 0.9),
            SubtitleEntry(startMs: 1000, endMs: 2000, text: "B", confidence: 0.9),
            SubtitleEntry(startMs: 2000, endMs: 3000, text: "C", confidence: 0.9),
        ]
    }

    func testMergeDisabledOnLastEntry() {
        let entries = makeEntries()
        // 最后一条不能与下一条合并。
        XCTAssertFalse(TranscriptRowAvailability.canMerge(entry: entries[2], in: entries, accessMode: .editable))
        // 中间条目可合并。
        XCTAssertTrue(TranscriptRowAvailability.canMerge(entry: entries[1], in: entries, accessMode: .editable))
    }

    func testSplitMergeDisabledInReadOnlyMode() {
        let entries = makeEntries()
        XCTAssertFalse(TranscriptRowAvailability.canSplit(entry: entries[0], in: entries, accessMode: .readOnly))
        XCTAssertFalse(TranscriptRowAvailability.canMerge(entry: entries[0], in: entries, accessMode: .readOnly))
        XCTAssertTrue(TranscriptRowAvailability.canSplit(entry: entries[0], in: entries, accessMode: .editable))
    }

    func testSplitDisabledForEmptyTextOrTooShort() {
        let entries = [SubtitleEntry(startMs: 0, endMs: 1000, text: "", confidence: 0.9)]
        // 空文本不可拆（editor.split 语义：无内容可拆）。
        XCTAssertFalse(TranscriptRowAvailability.canSplit(entry: entries[0], in: entries, accessMode: .editable))
    }
}
