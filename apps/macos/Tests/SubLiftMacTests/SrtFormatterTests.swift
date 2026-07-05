import Foundation
import Testing
@testable import SubLiftMac

/// feat-024：SrtFormatter 纯函数单测。
///
/// 覆盖空列表、单条/多条字幕、时间码边界、非法时间、编辑后 roundtrip。
@MainActor
struct SrtFormatterTests {

    // MARK: - format

    @Test
    func format_emptyEntries_returnsEmptyString() throws {
        let result = try SrtFormatter.format(entries: [])
        #expect(result == "")
    }

    @Test
    func format_singleEntry() throws {
        let entries = [
            SubtitleEntryData(startMs: 1000, endMs: 2500, text: "你好世界", confidence: 0.9)
        ]
        let result = try SrtFormatter.format(entries: entries)
        #expect(result == "1\n00:00:01,000 --> 00:00:02,500\n你好世界\n")
    }

    @Test
    func format_multipleEntries_separatedByBlankLine() throws {
        let entries = [
            SubtitleEntryData(startMs: 0, endMs: 1000, text: "第一条", confidence: 0.9),
            SubtitleEntryData(startMs: 1000, endMs: 2000, text: "第二条", confidence: 0.85),
        ]
        let result = try SrtFormatter.format(entries: entries)
        let expected = """
            1
            00:00:00,000 --> 00:00:01,000
            第一条

            2
            00:00:01,000 --> 00:00:02,000
            第二条

            """
        #expect(result == expected)
    }

    @Test
    func format_emptyText_preserved() throws {
        let entries = [
            SubtitleEntryData(startMs: 0, endMs: 1000, text: "", confidence: 0.9)
        ]
        let result = try SrtFormatter.format(entries: entries)
        #expect(result == "1\n00:00:00,000 --> 00:00:01,000\n\n")
    }

    @Test
    func format_textWithNewlines_preserved() throws {
        let entries = [
            SubtitleEntryData(startMs: 0, endMs: 1000, text: "第一行\n第二行", confidence: 0.9)
        ]
        let result = try SrtFormatter.format(entries: entries)
        #expect(result == "1\n00:00:00,000 --> 00:00:01,000\n第一行\n第二行\n")
    }

    @Test
    func format_invalidRange_throws() {
        let entries = [
            SubtitleEntryData(startMs: 1000, endMs: 500, text: "错误", confidence: 0.9)
        ]
        #expect(throws: SrtFormatError.invalidRange(startMs: 1000, endMs: 500)) {
            try SrtFormatter.format(entries: entries)
        }
    }

    @Test
    func format_negativeStart_throws() {
        let entries = [
            SubtitleEntryData(startMs: -100, endMs: 500, text: "错误", confidence: 0.9)
        ]
        #expect(throws: SrtFormatError.negativeTimestamp(-100)) {
            try SrtFormatter.format(entries: entries)
        }
    }

    @Test
    func format_negativeEnd_throws() {
        let entries = [
            SubtitleEntryData(startMs: 0, endMs: -100, text: "错误", confidence: 0.9)
        ]
        #expect(throws: SrtFormatError.negativeTimestamp(-100)) {
            try SrtFormatter.format(entries: entries)
        }
    }

    // MARK: - formatTimestamp

    @Test
    func formatTimestamp_zero() throws {
        #expect(try SrtFormatter.formatTimestamp(0) == "00:00:00,000")
    }

    @Test
    func formatTimestamp_millisecondsOnly() throws {
        #expect(try SrtFormatter.formatTimestamp(500) == "00:00:00,500")
    }

    @Test
    func formatTimestamp_secondsAndMilliseconds() throws {
        #expect(try SrtFormatter.formatTimestamp(61500) == "00:01:01,500")
    }

    @Test
    func formatTimestamp_allFields() throws {
        #expect(try SrtFormatter.formatTimestamp(3661500) == "01:01:01,500")
    }

    @Test
    func formatTimestamp_over24Hours_noRollover() throws {
        #expect(try SrtFormatter.formatTimestamp(90000000) == "25:00:00,000")
    }

    @Test
    func formatTimestamp_negative_throws() {
        #expect(throws: SrtFormatError.negativeTimestamp(-1)) {
            try SrtFormatter.formatTimestamp(-1)
        }
    }

    // MARK: - roundtrip with editor

    @Test
    func format_reflectsTextEdit() throws {
        let editor = SubtitleEditor()
        editor.load([
            SubtitleEntryData(startMs: 0, endMs: 1000, text: "原始", confidence: 0.9)
        ])
        let id = editor.entries[0].id
        editor.updateText(at: id, text: "修改后")

        let result = try SrtFormatter.format(entries: editor.exportEntries())
        #expect(result == "1\n00:00:00,000 --> 00:00:01,000\n修改后\n")
    }

    @Test
    func format_reflectsMerge() throws {
        let editor = SubtitleEditor()
        editor.load([
            SubtitleEntryData(startMs: 0, endMs: 1000, text: "你好", confidence: 0.9),
            SubtitleEntryData(startMs: 1000, endMs: 2000, text: "世界", confidence: 0.85)
        ])
        editor.merge(at: 0)

        let result = try SrtFormatter.format(entries: editor.exportEntries())
        #expect(result == "1\n00:00:00,000 --> 00:00:02,000\n你好 世界\n")
    }

    @Test
    func format_reflectsSplit() throws {
        let editor = SubtitleEditor()
        editor.load([
            SubtitleEntryData(startMs: 0, endMs: 2000, text: "原始", confidence: 0.9)
        ])
        editor.split(at: 0)

        let result = try SrtFormatter.format(entries: editor.exportEntries())
        let expected = """
            1
            00:00:00,000 --> 00:00:01,000
            原始

            2
            00:00:01,000 --> 00:00:02,000
            

            """
        #expect(result == expected)
    }

    // MARK: - 真实文件 I/O

    @Test
    func format_writeToFile_roundtrip() throws {
        let entries = [
            SubtitleEntryData(startMs: 0, endMs: 1000, text: "真实文件写入测试", confidence: 0.9),
            SubtitleEntryData(startMs: 1000, endMs: 2500, text: "第二行字幕", confidence: 0.85)
        ]
        let srt = try SrtFormatter.format(entries: entries)
        let url = FileManager.default.temporaryDirectory
            .appendingPathComponent("sublift-test-\(UUID().uuidString).srt")

        try srt.write(to: url, atomically: true, encoding: .utf8)
        defer { try? FileManager.default.removeItem(at: url) }

        let read = try String(contentsOf: url, encoding: .utf8)
        #expect(read == srt)
        #expect(FileManager.default.fileExists(atPath: url.path))
    }

    /// 生成一个固定路径的真实 SRT 文件，用于手动在 IINA / VLC 中验证加载。
    /// 文件路径：/tmp/sublift-real-export-test.srt
    @Test
    func format_writeRealSampleForManualVerification() throws {
        let entries = [
            SubtitleEntryData(startMs: 0, endMs: 2500, text: "这是第一条字幕", confidence: 0.95),
            SubtitleEntryData(startMs: 3000, endMs: 5500, text: "这是第二条字幕，带换行\n第二行文本", confidence: 0.92),
            SubtitleEntryData(startMs: 6000, endMs: 9000, text: "第三条字幕，测试播放器加载", confidence: 0.9)
        ]
        let srt = try SrtFormatter.format(entries: entries)
        let url = URL(fileURLWithPath: "/tmp/sublift-real-export-test.srt")
        try srt.write(to: url, atomically: true, encoding: .utf8)
        #expect(FileManager.default.fileExists(atPath: url.path))
    }
}
