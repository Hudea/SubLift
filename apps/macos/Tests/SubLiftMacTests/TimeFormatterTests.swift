import Foundation
import Testing
@testable import SubLiftMac

/// feat-017：TimeFormatter.formatMs 纯函数单测（swift-testing 轨）。
struct TimeFormatterTests {

    @Test
    func zero() {
        #expect(TimeFormatter.formatMs(0) == "00:00:00.000")
    }

    @Test
    func millisecondsOnly() {
        #expect(TimeFormatter.formatMs(999) == "00:00:00.999")
    }

    @Test
    func oneSecondBoundary() {
        #expect(TimeFormatter.formatMs(1000) == "00:00:01.000")
    }

    @Test
    func justBelowOneSecond() {
        #expect(TimeFormatter.formatMs(59999) == "00:00:59.999")
    }

    @Test
    func oneMinute() {
        #expect(TimeFormatter.formatMs(60_000) == "00:01:00.000")
    }

    @Test
    func oneHour() {
        #expect(TimeFormatter.formatMs(3_600_000) == "01:00:00.000")
    }

    @Test
    func tenHours() {
        #expect(TimeFormatter.formatMs(36_000_000) == "10:00:00.000")
    }

    @Test
    func complexTime() {
        // 1h 23m 45s 678ms
        let ms = 3_600_000 + 23 * 60_000 + 45_000 + 678
        #expect(TimeFormatter.formatMs(ms) == "01:23:45.678")
    }

    @Test
    func negativeClampsToZero() {
        #expect(TimeFormatter.formatMs(-1) == "00:00:00.000")
        #expect(TimeFormatter.formatMs(-1000) == "00:00:00.000")
    }

    @Test
    func moreThan24HoursStillFormats() {
        // 25h = 90_000_000 ms
        #expect(TimeFormatter.formatMs(90_000_000) == "25:00:00.000")
    }
}
