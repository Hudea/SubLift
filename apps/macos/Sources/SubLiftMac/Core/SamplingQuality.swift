import Foundation

/// 产品侧采样密度档位（UI 只展示中文名，不暴露具体 sample_fps）。
///
/// 映射到 Python / IPC 的 `fps`（时间轴采样率，与源视频帧率解耦）：
/// - 快速 → 5（当前默认与固定 GT 回归锚点）
/// - 平衡 → 8
/// - 精细 → 12（用户可选上限）
public enum SamplingQuality: String, CaseIterable, Identifiable, Codable, Sendable {
    case fast
    case balanced
    case fine

    public var id: String { rawValue }

    /// 传给 IPC / Pipeline 的采样帧率。
    public var sampleFps: Int {
        switch self {
        case .fast: return 5
        case .balanced: return 8
        case .fine: return 12
        }
    }

    /// 界面展示名（不显示数字）。
    public var displayName: String {
        switch self {
        case .fast: return "快速"
        case .balanced: return "平衡"
        case .fine: return "精细"
        }
    }

    /// 简短说明，用于 help / 设置页。
    public var helpText: String {
        switch self {
        case .fast: return "更快完成，适合常规字幕"
        case .balanced: return "速度与短字幕召回更均衡"
        case .fine: return "更高采样密度，处理更慢"
        }
    }
}
