import XCTest
@testable import SubLiftMac

/// 10310：Timeline 确定性时间几何测试。
final class TimelineGeometryTests: XCTestCase {

    private let range = TimelineGeometry.Range(startMs: 0, durationMs: 10_000)

    func testEntryMappingIsDeterministic() {
        // 0–10s 范围、100pt 宽：5s → 50pt。
        let x = TimelineGeometry.xPosition(
            timeMs: 5_000,
            range: range,
            width: 100
        )
        XCTAssertEqual(x, 50, accuracy: 0.001)
    }

    func testEntryStartAndEndMapCorrectly() {
        let start = TimelineGeometry.xPosition(timeMs: 2_000, range: range, width: 100)
        let end = TimelineGeometry.xPosition(timeMs: 8_000, range: range, width: 100)
        XCTAssertEqual(start, 20, accuracy: 0.001)
        XCTAssertEqual(end, 80, accuracy: 0.001)
    }

    func testMappingClampsOutOfRange() {
        // 超出范围的时间 clamp 到 [0, width]。
        XCTAssertEqual(TimelineGeometry.xPosition(timeMs: -100, range: range, width: 100), 0, accuracy: 0.001)
        XCTAssertEqual(TimelineGeometry.xPosition(timeMs: 99_000, range: range, width: 100), 100, accuracy: 0.001)
    }

    func testZeroDurationRangeFallsBackToZero() {
        let zeroRange = TimelineGeometry.Range(startMs: 0, durationMs: 0)
        XCTAssertEqual(TimelineGeometry.xPosition(timeMs: 1_000, range: zeroRange, width: 100), 0)
    }

    func testShortEntriesGetMinHitWidth() {
        // 极短条目（0.1s）在 100pt 宽下宽度 < 最小命中宽度 → 使用最小宽度。
        let start = 4_000, end = 4_100
        let width = TimelineGeometry.entryWidth(startMs: start, endMs: end, range: range, trackWidth: 100)
        XCTAssertGreaterThanOrEqual(width, TimelineGeometry.minHitWidth - 0.001)
        // 正常宽度条目不受影响。
        let normal = TimelineGeometry.entryWidth(startMs: 2_000, endMs: 8_000, range: range, trackWidth: 100)
        XCTAssertEqual(normal, 60, accuracy: 0.001)
    }

    func testPlayheadPosition() {
        let x = TimelineGeometry.xPosition(timeMs: 3_000, range: range, width: 100)
        XCTAssertEqual(x, 30, accuracy: 0.001)
    }

    func testRangeFromEntries() {
        let entries = [
            SubtitleEntry(startMs: 1_000, endMs: 2_000, text: "a", confidence: 0.9),
            SubtitleEntry(startMs: 5_000, endMs: 9_000, text: "b", confidence: 0.9),
        ]
        let r = TimelineGeometry.range(from: entries)
        XCTAssertEqual(r.startMs, 1_000)
        XCTAssertEqual(r.durationMs, 8_000)
    }

    func testEmptyEntriesRange() {
        let r = TimelineGeometry.range(from: [])
        XCTAssertEqual(r.durationMs, 0)
    }

    func testMappingPrefersVideoDurationRange() {
        // 验收合同：有视频时长时按视频时长映射（0 → durationMs），回退 entries 范围。
        let videoRange = TimelineGeometry.Range(startMs: 0, durationMs: 60_000)
        let x = TimelineGeometry.xPosition(timeMs: 30_000, range: videoRange, width: 100)
        XCTAssertEqual(x, 50, accuracy: 0.001)
        // 视频 60s、条目仅占 0–12s：条目仍映射到全程（30s → 中点）。
        let entries = [
            SubtitleEntry(startMs: 1_000, endMs: 3_000, text: "a", confidence: 0.9),
            SubtitleEntry(startMs: 5_000, endMs: 12_000, text: "b", confidence: 0.9),
        ]
        let entryStartX = TimelineGeometry.xPosition(timeMs: entries[1].startMs, range: videoRange, width: 100)
        XCTAssertEqual(entryStartX, 5000.0 / 60_000.0 * 100, accuracy: 0.01)
    }
}

/// 10310：Previous/Next 字幕导航纯逻辑。
final class TimelineNavigationTests: XCTestCase {

    private func makeEntries() -> [SubtitleEntry] {
        [
            SubtitleEntry(startMs: 1_000, endMs: 3_000, text: "A", confidence: 0.9),
            SubtitleEntry(startMs: 5_000, endMs: 7_000, text: "B", confidence: 0.9),
            SubtitleEntry(startMs: 9_000, endMs: 12_000, text: "C", confidence: 0.9),
        ]
    }

    func testNextFromBeforeFirstJumpsToFirst() {
        let entries = makeEntries()
        let next = TimelineNavigation.nextEntryIndex(currentMs: 0, entries: entries)
        XCTAssertEqual(next, 0)
    }

    func testNextFromInsideEntryJumpsToFollowing() {
        let entries = makeEntries()
        // 当前在 A 内（2s）→ 下一条是 B（索引 1）。
        let next = TimelineNavigation.nextEntryIndex(currentMs: 2_000, entries: entries)
        XCTAssertEqual(next, 1)
    }

    func testNextFromAfterLastReturnsNil() {
        let entries = makeEntries()
        XCTAssertNil(TimelineNavigation.nextEntryIndex(currentMs: 13_000, entries: entries))
    }

    func testPreviousFromAfterLastJumpsToLast() {
        let entries = makeEntries()
        let prev = TimelineNavigation.previousEntryIndex(currentMs: 13_000, entries: entries)
        XCTAssertEqual(prev, 2)
    }

    func testPreviousFromInsideFirstReturnsNil() {
        let entries = makeEntries()
        XCTAssertNil(TimelineNavigation.previousEntryIndex(currentMs: 1_500, entries: entries))
    }

    func testPreviousStrictlyBeforeCurrentEntry() {
        let entries = makeEntries()
        // 当前在 B 内（6s）→ 前一条是 A（索引 0，严格在其开始之前）。
        let prev = TimelineNavigation.previousEntryIndex(currentMs: 6_000, entries: entries)
        XCTAssertEqual(prev, 0)
    }

    func testEmptyEntriesNavigation() {
        XCTAssertNil(TimelineNavigation.nextEntryIndex(currentMs: 0, entries: []))
        XCTAssertNil(TimelineNavigation.previousEntryIndex(currentMs: 0, entries: []))
    }
}

/// 10414：Timeline 范围标签使用紧凑、单行友好的时间文本。
final class TimelineRangePresentationTests: XCTestCase {

    func testSubHourRangeOmitsMillisecondsAndRedundantHour() {
        XCTAssertEqual(
            TimelineRangePresentation.text(startMs: 1_250, endMs: 189_999),
            "0:01–3:09"
        )
    }

    func testHourRangeKeepsHourContext() {
        XCTAssertEqual(
            TimelineRangePresentation.text(startMs: 3_723_000, endMs: 7_204_000),
            "1:02:03–2:00:04"
        )
    }

    func testEmptyRangeUsesPlaceholder() {
        XCTAssertEqual(TimelineRangePresentation.text(startMs: 0, endMs: 0), "—")
    }
}
