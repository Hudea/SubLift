import Foundation

/// feat-021：可编辑字幕条目模型。
///
/// 与 `SubtitleEntryData`（IPC 不可变 struct）解耦：
/// - `SubtitleEntryData` 用于 IPC 传输（let 字段，无 id）
/// - `SubtitleEntry` 用于 UI 编辑（var 字段，带 UUID）
///
/// 从 IPC 数据通过 `init(_:)` 转换；编辑后通过 `toData()` 转回。
public struct SubtitleEntry: Identifiable, Equatable {
    public let id: UUID
    public var startMs: Int
    public var endMs: Int
    public var text: String
    public var confidence: Double

    public init(id: UUID = UUID(), startMs: Int, endMs: Int, text: String, confidence: Double) {
        self.id = id
        self.startMs = startMs
        self.endMs = endMs
        self.text = text
        self.confidence = confidence
    }

    /// 从 IPC 数据转换。
    public init(_ data: SubtitleEntryData) {
        self.init(
            startMs: data.startMs,
            endMs: data.endMs,
            text: data.text,
            confidence: data.confidence
        )
    }

    /// feat-024 预留：转回 IPC 数据格式。
    public func toData() -> SubtitleEntryData {
        SubtitleEntryData(startMs: startMs, endMs: endMs, text: text, confidence: confidence)
    }

    /// 时间是否落在区间 [startMs, endMs) 内。
    public func contains(ms: Int) -> Bool {
        ms >= startMs && ms < endMs
    }
}
