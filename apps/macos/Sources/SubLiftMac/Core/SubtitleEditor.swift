import Foundation

/// feat-021：字幕编辑器（可变状态）。
///
/// 从 `SubtitleExtractor.entries`（只读 `[SubtitleEntryData]`）拷贝一份可编辑副本，
/// 提供文本编辑、时间步进、合并/拆分等操作。编辑后通过 `exportEntries()` 转回 IPC 格式，
/// 为 feat-024 SrtFormatter 提供输入。
///
/// 与 SubtitleExtractor 解耦：extractor 负责「提取」，editor 负责「编辑」。
@MainActor
final class SubtitleEditor: ObservableObject {

    @Published private(set) var entries: [SubtitleEntry] = []
    @Published var selectedId: UUID?
    /// 当前播放高亮条目 id（由 player.currentMs 节流计算，避免 10Hz 全列表重绘）。
    @Published private(set) var currentId: UUID?

    /// 从 IPC 数据加载可编辑副本。
    func load(_ data: [SubtitleEntryData]) {
        entries = data.map(SubtitleEntry.init)
        selectedId = nil
        currentId = nil
    }

    /// 增量追加单条字幕（流式提取过程中实时显示）。
    /// finalize 后 load() 会全量替换，增量条目被丢弃。
    func appendIncremental(_ data: SubtitleEntryData) {
        entries.append(SubtitleEntry(data))
    }

    /// 清空编辑器（新视频导入时）。
    func clear() {
        entries = []
        selectedId = nil
        currentId = nil
    }

    /// 更新当前高亮条目（仅当高亮条目变化时才触发 @Published）。
    func updateCurrent(atMs ms: Int) {
        let newId = entries.first { $0.contains(ms: ms) }?.id
        if newId != currentId {
            currentId = newId
        }
    }

    // MARK: - 文本编辑

    func updateText(at id: UUID, text: String) {
        guard let idx = entries.firstIndex(where: { $0.id == id }) else { return }
        entries[idx].text = text
    }

    // MARK: - 合并

    /// 合并 index 与 index+1。
    /// - startMs 取前者 startMs
    /// - endMs 取后者 endMs
    /// - text 用空格拼接
    /// - Returns: 是否成功（index 越界或为最后一条时返回 false）
    @discardableResult
    func merge(at index: Int) -> Bool {
        guard index >= 0, index < entries.count - 1 else { return false }
        let first = entries[index]
        let second = entries[index + 1]
        let mergedIds: Set<UUID> = [first.id, second.id]
        let merged = SubtitleEntry(
            id: first.id,
            startMs: first.startMs,
            endMs: second.endMs,
            text: first.text + " " + second.text,
            confidence: max(first.confidence, second.confidence)
        )
        entries[index] = merged
        entries.remove(at: index + 1)
        if let selectedId, mergedIds.contains(selectedId) {
            self.selectedId = merged.id
        }
        if let currentId, mergedIds.contains(currentId) {
            self.currentId = merged.id
        }
        reconcileSelectionAndCurrent()
        return true
    }

    // MARK: - 拆分

    /// 在 index 处拆分为两条。
    /// - midMs = (startMs + endMs) / 2
    /// - 前段：startMs 不变，endMs = midMs，text 保留
    /// - 后段：startMs = midMs，endMs 不变，text = ""
    /// - Returns: 是否成功（index 越界或区间不足 2ms 时返回 false）
    @discardableResult
    func split(at index: Int) -> Bool {
        guard index >= 0, index < entries.count else { return false }
        let original = entries[index]
        guard original.endMs - original.startMs >= 2 else { return false }
        let midMs = (original.startMs + original.endMs) / 2
        let first = SubtitleEntry(
            id: original.id,
            startMs: original.startMs,
            endMs: midMs,
            text: original.text,
            confidence: original.confidence
        )
        let second = SubtitleEntry(
            startMs: midMs,
            endMs: original.endMs,
            text: "",
            confidence: original.confidence
        )
        entries[index] = first
        entries.insert(second, at: index + 1)
        reconcileSelectionAndCurrent()
        return true
    }

    // MARK: - 查询

    /// 当前播放时间落在区间内的条目（高亮用）。
    func currentEntry(atMs ms: Int) -> SubtitleEntry? {
        entries.first { $0.contains(ms: ms) }
    }

    // MARK: - 导出（feat-024 预留）

    /// 转回 IPC 数据格式，供 SrtFormatter 使用。
    func exportEntries() -> [SubtitleEntryData] {
        entries.map { $0.toData() }
    }

    // MARK: - 状态修正

    private func reconcileSelectionAndCurrent() {
        let validIds = Set(entries.map(\.id))
        if let selectedId, !validIds.contains(selectedId) {
            self.selectedId = nil
        }
        if let currentId, !validIds.contains(currentId) {
            self.currentId = nil
        }
    }
}
