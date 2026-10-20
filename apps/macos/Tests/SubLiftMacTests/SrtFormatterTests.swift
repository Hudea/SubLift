import Foundation
import Testing
@testable import SubLiftMac

/// Phase 17 D02：SRT canonical policy 测试。
///
/// 规范见 contracts/fixtures/srt-cases.json：按 start 升序、跳过纯空白、
/// 负数/逆序钳制、序号按输出块连续、末尾一个换行。
@MainActor
struct SrtFormatterTests {

    @Test
    func format_emptyEntries_returnsEmptyString() {
        #expect(SrtFormatter.format(entries: []) == "")
    }

    @Test
    func format_singleEntry() {
        let entries = [
            SubtitleEntryData(startMs: 1000, endMs: 2500, text: "你好世界", confidence: 0.9)
        ]
        #expect(SrtFormatter.format(entries: entries) == "1\n00:00:01,000 --> 00:00:02,500\n你好世界\n\n")
    }

    @Test
    func format_multipleEntries_separatedByBlankLine() {
        let entries = [
            SubtitleEntryData(startMs: 0, endMs: 1000, text: "第一条", confidence: 0.9),
            SubtitleEntryData(startMs: 1000, endMs: 2000, text: "第二条", confidence: 0.85),
        ]
        let expected = "1\n00:00:00,000 --> 00:00:01,000\n第一条\n\n"
            + "2\n00:00:01,000 --> 00:00:02,000\n第二条\n\n"
        #expect(SrtFormatter.format(entries: entries) == expected)
    }

    @Test
    func format_sortsByStartAndRenumbers() {
        let entries = [
            SubtitleEntryData(startMs: 2000, endMs: 3000, text: "B", confidence: 0.9),
            SubtitleEntryData(startMs: 1000, endMs: 1500, text: "A", confidence: 0.8),
        ]
        let expected = "1\n00:00:01,000 --> 00:00:01,500\nA\n\n"
            + "2\n00:00:02,000 --> 00:00:03,000\nB\n\n"
        #expect(SrtFormatter.format(entries: entries) == expected)
    }

    @Test
    func format_skipsBlankAndRenumbers() {
        let entries = [
            SubtitleEntryData(startMs: 0, endMs: 1000, text: "   \n\t", confidence: 0.9),
            SubtitleEntryData(startMs: 1000, endMs: 2000, text: "保留", confidence: 0.8),
        ]
        #expect(SrtFormatter.format(entries: entries) == "1\n00:00:01,000 --> 00:00:02,000\n保留\n\n")
    }

    @Test
    func format_textWithNewlines_preserved() {
        let entries = [
            SubtitleEntryData(startMs: 0, endMs: 1000, text: "第一行\n第二行", confidence: 0.9)
        ]
        #expect(SrtFormatter.format(entries: entries) == "1\n00:00:00,000 --> 00:00:01,000\n第一行\n第二行\n\n")
    }

    @Test
    func format_invalidRange_clampsToStart() {
        let entries = [
            SubtitleEntryData(startMs: 1000, endMs: 500, text: "错误", confidence: 0.9)
        ]
        #expect(SrtFormatter.format(entries: entries) == "1\n00:00:01,000 --> 00:00:01,000\n错误\n\n")
    }

    @Test
    func format_negativeStart_clampsToZero() {
        let entries = [
            SubtitleEntryData(startMs: -100, endMs: 500, text: "错误", confidence: 0.9)
        ]
        #expect(SrtFormatter.format(entries: entries) == "1\n00:00:00,000 --> 00:00:00,500\n错误\n\n")
    }

    @Test
    func format_negativeEnd_clampsToZero() {
        let entries = [
            SubtitleEntryData(startMs: 0, endMs: -100, text: "错误", confidence: 0.9)
        ]
        #expect(SrtFormatter.format(entries: entries) == "1\n00:00:00,000 --> 00:00:00,000\n错误\n\n")
    }

    @Test
    func formatTimestamp_boundaries() {
        #expect(SrtFormatter.formatTimestamp(0) == "00:00:00,000")
        #expect(SrtFormatter.formatTimestamp(500) == "00:00:00,500")
        #expect(SrtFormatter.formatTimestamp(61500) == "00:01:01,500")
        #expect(SrtFormatter.formatTimestamp(3661500) == "01:01:01,500")
        #expect(SrtFormatter.formatTimestamp(90000000) == "25:00:00,000")
        #expect(SrtFormatter.formatTimestamp(-1) == "00:00:00,000")
    }

    @Test
    func format_reflectsTextEdit() {
        let editor = SubtitleEditor()
        editor.load([
            SubtitleEntryData(startMs: 0, endMs: 1000, text: "原始", confidence: 0.9)
        ])
        let id = editor.entries[0].id
        editor.updateText(at: id, text: "修改后")

        #expect(SrtFormatter.format(entries: editor.exportEntries())
            == "1\n00:00:00,000 --> 00:00:01,000\n修改后\n\n")
    }

    @Test
    func format_reflectsMerge() {
        let editor = SubtitleEditor()
        editor.load([
            SubtitleEntryData(startMs: 0, endMs: 1000, text: "你好", confidence: 0.9),
            SubtitleEntryData(startMs: 1000, endMs: 2000, text: "世界", confidence: 0.85),
        ])
        editor.merge(at: 0)

        #expect(SrtFormatter.format(entries: editor.exportEntries())
            == "1\n00:00:00,000 --> 00:00:02,000\n你好 世界\n\n")
    }

    @Test
    func format_reflectsSplit_skipsEmptyHalf() {
        let editor = SubtitleEditor()
        editor.load([
            SubtitleEntryData(startMs: 0, endMs: 2000, text: "原始", confidence: 0.9)
        ])
        editor.split(at: 0)

        #expect(SrtFormatter.format(entries: editor.exportEntries())
            == "1\n00:00:00,000 --> 00:00:01,000\n原始\n\n")
    }

    @Test
    func format_writeToFile_roundtrip() throws {
        let entries = [
            SubtitleEntryData(startMs: 0, endMs: 1000, text: "真实文件写入测试", confidence: 0.9),
            SubtitleEntryData(startMs: 1000, endMs: 2500, text: "第二行字幕", confidence: 0.85),
        ]
        let srt = SrtFormatter.format(entries: entries)
        let url = FileManager.default.temporaryDirectory
            .appendingPathComponent("sublift-test-\(UUID().uuidString).srt")

        try srt.write(to: url, atomically: true, encoding: .utf8)
        defer { try? FileManager.default.removeItem(at: url) }

        let read = try String(contentsOf: url, encoding: .utf8)
        #expect(read == srt)
    }

    @Test
    func format_writeRealSampleForManualVerification() throws {
        let entries = [
            SubtitleEntryData(startMs: 0, endMs: 2500, text: "这是第一条字幕", confidence: 0.95),
            SubtitleEntryData(startMs: 3000, endMs: 5500, text: "这是第二条字幕，带换行\n第二行文本", confidence: 0.92),
            SubtitleEntryData(startMs: 6000, endMs: 9000, text: "第三条字幕，测试播放器加载", confidence: 0.9),
        ]
        let srt = SrtFormatter.format(entries: entries)
        let url = URL(fileURLWithPath: "/tmp/sublift-real-export-test.srt")
        try srt.write(to: url, atomically: true, encoding: .utf8)
        #expect(FileManager.default.fileExists(atPath: url.path))
    }
}
