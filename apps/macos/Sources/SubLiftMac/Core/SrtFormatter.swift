import Foundation

/// SRT 字幕格式化器（Phase 17 D02 canonical policy）。
///
/// 与 contracts/fixtures/srt-cases.json 对齐：
/// - 按 startMs 升序（同 start 时按 endMs）
/// - 跳过纯空白文本
/// - start = max(0, start)，end = max(start, end)
/// - 序号按实际输出块连续
/// - 块间空行，末尾保留一个换行
enum SrtFormatter {

    static func format(entries: [SubtitleEntryData]) -> String {
        let sorted = entries.sorted { lhs, rhs in
            if lhs.startMs != rhs.startMs {
                return lhs.startMs < rhs.startMs
            }
            return lhs.endMs < rhs.endMs
        }

        var blocks: [String] = []
        for entry in sorted {
            let text = entry.text
            guard !text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty else {
                continue
            }
            let start = max(0, entry.startMs)
            let end = max(start, entry.endMs)
            let index = blocks.count + 1
            blocks.append(
                "\(index)\n\(formatTimestamp(start)) --> \(formatTimestamp(end))\n\(text)"
            )
        }

        return blocks.isEmpty ? "" : blocks.joined(separator: "\n\n") + "\n\n"
    }

    /// 将毫秒时间戳格式化为 SRT 时间码 `HH:MM:SS,mmm`；负数按 0 处理。
    static func formatTimestamp(_ ms: Int) -> String {
        let safe = max(0, ms)
        let h = safe / 3_600_000
        let rem = safe % 3_600_000
        let m = rem / 60_000
        let rem2 = rem % 60_000
        let s = rem2 / 1_000
        let msPart = rem2 % 1_000
        return String(format: "%02d:%02d:%02d,%03d", h, m, s, msPart)
    }
}
