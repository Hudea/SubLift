import CoreGraphics
import Foundation

/// 10310：Timeline 确定性时间几何（纯函数）。
///
/// 时间 → x 的映射是确定性的：`x = (t - rangeStart) / rangeDuration * width`，
/// 超出范围 clamp 到 [0, width]；极短条目使用最小命中宽度。
enum TimelineGeometry {

    /// 条带高度合同（28–40pt）。
    static let barHeight: CGFloat = 32

    /// 极短条目的最小命中宽度（pt）。
    static let minHitWidth: CGFloat = 8

    struct Range: Equatable {
        let startMs: Int
        let durationMs: Int

        static let empty = Range(startMs: 0, durationMs: 0)
    }

    /// 从 entries 推导时间范围（首个 start 到最后一个 end）。
    static func range(from entries: [SubtitleEntry]) -> Range {
        guard let first = entries.first, let last = entries.last else { return .empty }
        return Range(startMs: first.startMs, durationMs: max(0, last.endMs - first.startMs))
    }

    /// 时间点 → x 坐标（确定性映射，越界 clamp）。
    static func xPosition(timeMs: Int, range: Range, width: CGFloat) -> CGFloat {
        guard range.durationMs > 0, width > 0 else { return 0 }
        let t = Double(max(0, timeMs - range.startMs))
        let fraction = min(1, max(0, t / Double(range.durationMs)))
        return CGFloat(fraction) * width
    }

    /// 条目宽度（至少最小命中宽度）。
    static func entryWidth(startMs: Int, endMs: Int, range: Range, trackWidth: CGFloat) -> CGFloat {
        let startX = xPosition(timeMs: startMs, range: range, width: trackWidth)
        let endX = xPosition(timeMs: endMs, range: range, width: trackWidth)
        return max(minHitWidth, endX - startX)
    }
}

/// 10310：Previous/Next 字幕导航纯逻辑。
enum TimelineNavigation {

    /// 当前播放时间 → 下一条字幕索引（严格在当前开始之后；无则 nil）。
    static func nextEntryIndex(currentMs: Int, entries: [SubtitleEntry]) -> Int? {
        entries.firstIndex { $0.startMs > currentMs }
    }

    /// 当前播放时间 → 前一条字幕索引（跳过当前正在显示的字幕，取之前最近一条；无则 nil）。
    static func previousEntryIndex(currentMs: Int, entries: [SubtitleEntry]) -> Int? {
        entries.lastIndex { entry in
            entry.startMs < currentMs && !(currentMs < entry.endMs)
        }
    }
}
