import Foundation

/// 时间格式化纯函数（feat-017 单测目标）。
enum TimeFormatter {
    /// 把毫秒格式化为 `HH:MM:SS.mmm`。
    ///
    /// - Parameter ms: 毫秒数。负数 clamp 为 0。
    /// - Returns: 格式化字符串，如 `00:00:00.000`、`01:00:00.000`。
    static func formatMs(_ ms: Int) -> String {
        let total = max(0, ms)
        let totalSeconds = total / 1000
        let millis = total % 1000
        let seconds = totalSeconds % 60
        let totalMinutes = totalSeconds / 60
        let minutes = totalMinutes % 60
        let hours = totalMinutes / 60
        return String(format: "%02d:%02d:%02d.%03d", hours, minutes, seconds, millis)
    }
}
