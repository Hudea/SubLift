import Foundation

/// feat-024：SRT 字幕格式化器。
///
/// 纯函数，将内存中的字幕条目格式化为标准 SRT 文本。
/// 按 ADR-0007b，GUI 端直接在 Swift 端格式化，不走 IPC 往返 Python。
enum SrtFormatter {

    /// 将字幕条目格式化为 SRT 字符串。
    ///
    /// 格式规范：
    /// - 空列表返回空字符串
    /// - 序号从 1 开始，按列表顺序递增
    /// - 时间码格式为 `HH:MM:SS,mmm`
    /// - 条目之间以空行分隔
    /// - 文件末尾保留一个换行
    ///
    /// - Parameter entries: 字幕条目列表。
    /// - Returns: SRT 格式字符串。
    /// - Throws: `SrtFormatError` 当时间戳为负或 `endMs < startMs` 时。
    static func format(entries: [SubtitleEntryData]) throws -> String {
        guard !entries.isEmpty else { return "" }

        var blocks: [String] = []
        for (idx, entry) in entries.enumerated() {
            try validate(entry)
            let startTC = try formatTimestamp(entry.startMs)
            let endTC = try formatTimestamp(entry.endMs)
            blocks.append("\(idx + 1)\n\(startTC) --> \(endTC)\n\(entry.text)")
        }
        return blocks.joined(separator: "\n\n") + "\n"
    }

    /// 将毫秒时间戳格式化为 SRT 时间码 `HH:MM:SS,mmm`。
    ///
    /// - Parameter ms: 毫秒数，必须 >= 0。
    /// - Returns: 格式化字符串，如 `"01:01:01,500"`。
    /// - Throws: `SrtFormatError.negativeTimestamp` 当 ms 为负数时。
    static func formatTimestamp(_ ms: Int) throws -> String {
        if ms < 0 {
            throw SrtFormatError.negativeTimestamp(ms)
        }
        let h = ms / 3_600_000
        let rem = ms % 3_600_000
        let m = rem / 60_000
        let rem2 = rem % 60_000
        let s = rem2 / 1_000
        let msPart = rem2 % 1_000
        return String(format: "%02d:%02d:%02d,%03d", h, m, s, msPart)
    }

    // MARK: - Private

    private static func validate(_ entry: SubtitleEntryData) throws {
        if entry.startMs < 0 || entry.endMs < 0 {
            throw SrtFormatError.negativeTimestamp(min(entry.startMs, entry.endMs))
        }
        if entry.endMs < entry.startMs {
            throw SrtFormatError.invalidRange(startMs: entry.startMs, endMs: entry.endMs)
        }
    }
}

// MARK: - SrtFormatError

/// SRT 格式化错误。
enum SrtFormatError: LocalizedError, Equatable {
    case negativeTimestamp(Int)
    case invalidRange(startMs: Int, endMs: Int)

    var errorDescription: String? {
        switch self {
        case .negativeTimestamp(let ms):
            return "时间戳不能为负: \(ms)"
        case .invalidRange(let startMs, let endMs):
            return "结束时间不能小于开始时间: end_ms=\(endMs) < start_ms=\(startMs)"
        }
    }
}
