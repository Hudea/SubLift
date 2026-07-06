import Foundation
import Testing
@testable import SubLiftMac

/// feat-021：SubtitleEditor 纯函数单测（合并/拆分/编辑/边界）。
///
/// SubtitleEditor 是 @MainActor ObservableObject，测试需在 MainActor 上运行。
@MainActor
struct SubtitleEditorTests {

    // MARK: - 辅助

    private func sampleEntries() -> [SubtitleEntryData] {
        [
            SubtitleEntryData(startMs: 0, endMs: 1000, text: "你好", confidence: 0.9),
            SubtitleEntryData(startMs: 1000, endMs: 2000, text: "世界", confidence: 0.85),
            SubtitleEntryData(startMs: 2000, endMs: 3000, text: "测试", confidence: 0.8),
        ]
    }

    // MARK: - load / clear

    @Test
    func load_copiesEntries() {
        let editor = SubtitleEditor()
        editor.load(sampleEntries())
        #expect(editor.entries.count == 3)
        #expect(editor.entries[0].text == "你好")
        #expect(editor.entries[2].endMs == 3000)
    }

    @Test
    func load_generatesUUIDs() {
        let editor = SubtitleEditor()
        editor.load(sampleEntries())
        let ids = Set(editor.entries.map(\.id))
        #expect(ids.count == 3)
    }

    @Test
    func clear_emptiesEntries() {
        let editor = SubtitleEditor()
        editor.load(sampleEntries())
        editor.clear()
        #expect(editor.entries.isEmpty)
    }

    // MARK: - updateText

    @Test
    func updateText_changesText() {
        let editor = SubtitleEditor()
        editor.load(sampleEntries())
        let id = editor.entries[1].id
        editor.updateText(at: id, text: "新文本")
        #expect(editor.entries[1].text == "新文本")
    }

    @Test
    func updateText_unknownId_noop() {
        let editor = SubtitleEditor()
        editor.load(sampleEntries())
        editor.updateText(at: UUID(), text: "x")
        #expect(editor.entries.count == 3)
        #expect(editor.entries[0].text == "你好")
    }

    // MARK: - merge

    @Test
    func merge_twoEntries_combined() {
        let editor = SubtitleEditor()
        editor.load(sampleEntries())
        // [0]: 0-1000 "你好", [1]: 1000-2000 "世界"
        let result = editor.merge(at: 0)
        #expect(result == true)
        #expect(editor.entries.count == 2)
        #expect(editor.entries[0].startMs == 0)
        #expect(editor.entries[0].endMs == 2000)
        #expect(editor.entries[0].text == "你好 世界")
    }

    @Test
    func merge_lastIndex_returnsFalse() {
        let editor = SubtitleEditor()
        editor.load(sampleEntries())
        let result = editor.merge(at: 2)  // 最后一条
        #expect(result == false)
        #expect(editor.entries.count == 3)
    }

    @Test
    func merge_outOfBounds_returnsFalse() {
        let editor = SubtitleEditor()
        editor.load(sampleEntries())
        #expect(editor.merge(at: -1) == false)
        #expect(editor.merge(at: 99) == false)
    }

    @Test
    func merge_emptyList_returnsFalse() {
        let editor = SubtitleEditor()
        #expect(editor.merge(at: 0) == false)
    }

    @Test
    func merge_takesMaxConfidence() {
        let editor = SubtitleEditor()
        editor.load([
            SubtitleEntryData(startMs: 0, endMs: 1000, text: "A", confidence: 0.5),
            SubtitleEntryData(startMs: 1000, endMs: 2000, text: "B", confidence: 0.9),
        ])
        _ = editor.merge(at: 0)
        #expect(editor.entries[0].confidence == 0.9)
    }

    @Test
    func merge_preservesFirstId() {
        let editor = SubtitleEditor()
        editor.load(sampleEntries())
        let firstId = editor.entries[0].id
        _ = editor.merge(at: 0)
        #expect(editor.entries[0].id == firstId)
    }

    @Test
    func merge_selectedSecond_movesSelectionToMergedEntry() {
        let editor = SubtitleEditor()
        editor.load(sampleEntries())
        let firstId = editor.entries[0].id
        let secondId = editor.entries[1].id
        editor.selectedId = secondId
        _ = editor.merge(at: 0)
        #expect(editor.selectedId == firstId)
        let selectedIsValid = editor.selectedId.map { id in editor.entries.contains { $0.id == id } } ?? false
        #expect(selectedIsValid)
    }

    @Test
    func merge_currentSecond_movesCurrentToMergedEntry() {
        let editor = SubtitleEditor()
        editor.load(sampleEntries())
        let firstId = editor.entries[0].id
        editor.updateCurrent(atMs: 1500)
        #expect(editor.currentId == editor.entries[1].id)
        _ = editor.merge(at: 0)
        #expect(editor.currentId == firstId)
        let currentIsValid = editor.currentId.map { id in editor.entries.contains { $0.id == id } } ?? false
        #expect(currentIsValid)
    }

    // MARK: - split

    @Test
    func split_midPoint() {
        let editor = SubtitleEditor()
        editor.load([
            SubtitleEntryData(startMs: 0, endMs: 1000, text: "你好世界", confidence: 0.9),
        ])
        let result = editor.split(at: 0)
        #expect(result == true)
        #expect(editor.entries.count == 2)
        // midMs = (0 + 1000) / 2 = 500
        #expect(editor.entries[0].startMs == 0)
        #expect(editor.entries[0].endMs == 500)
        #expect(editor.entries[0].text == "你好世界")
        #expect(editor.entries[1].startMs == 500)
        #expect(editor.entries[1].endMs == 1000)
        #expect(editor.entries[1].text == "")
    }

    @Test
    func split_preservesConfidence() {
        let editor = SubtitleEditor()
        editor.load([
            SubtitleEntryData(startMs: 0, endMs: 1000, text: "A", confidence: 0.7),
        ])
        _ = editor.split(at: 0)
        #expect(editor.entries[0].confidence == 0.7)
        #expect(editor.entries[1].confidence == 0.7)
    }

    @Test
    func split_preservesOriginalIdForFirstHalf() {
        let editor = SubtitleEditor()
        editor.load(sampleEntries())
        let originalId = editor.entries[1].id
        _ = editor.split(at: 1)
        #expect(editor.entries[1].id == originalId)
        #expect(editor.entries[2].id != originalId)
    }

    @Test
    func split_selectedEntryStaysSelected() {
        let editor = SubtitleEditor()
        editor.load(sampleEntries())
        let originalId = editor.entries[1].id
        editor.selectedId = originalId
        _ = editor.split(at: 1)
        #expect(editor.selectedId == originalId)
        let selectedIsValid = editor.selectedId.map { id in editor.entries.contains { $0.id == id } } ?? false
        #expect(selectedIsValid)
    }

    @Test
    func split_tooShort_returnsFalse() {
        let editor = SubtitleEditor()
        editor.load([
            SubtitleEntryData(startMs: 100, endMs: 101, text: "X", confidence: 1.0),  // 区间 1ms
        ])
        let result = editor.split(at: 0)
        #expect(result == false)
        #expect(editor.entries.count == 1)
    }

    @Test
    func split_outOfBounds_returnsFalse() {
        let editor = SubtitleEditor()
        editor.load(sampleEntries())
        #expect(editor.split(at: -1) == false)
        #expect(editor.split(at: 99) == false)
    }

    @Test
    func split_emptyList_returnsFalse() {
        let editor = SubtitleEditor()
        #expect(editor.split(at: 0) == false)
    }

    // MARK: - currentEntry

    @Test
    func currentEntry_withinRange() {
        let editor = SubtitleEditor()
        editor.load(sampleEntries())
        // [0]: 0-1000, [1]: 1000-2000, [2]: 2000-3000
        #expect(editor.currentEntry(atMs: 500)?.text == "你好")
        #expect(editor.currentEntry(atMs: 1500)?.text == "世界")
        #expect(editor.currentEntry(atMs: 2500)?.text == "测试")
    }

    @Test
    func currentEntry_atBoundary() {
        let editor = SubtitleEditor()
        editor.load(sampleEntries())
        // startMs 是闭区间，endMs 是开区间
        #expect(editor.currentEntry(atMs: 1000)?.text == "世界")  // 落在 [1]
        #expect(editor.currentEntry(atMs: 3000) == nil)  // 超出最后一条
    }

    @Test
    func currentEntry_emptyList() {
        let editor = SubtitleEditor()
        #expect(editor.currentEntry(atMs: 0) == nil)
    }

    // MARK: - updateCurrent

    @Test
    func updateCurrent_setsCurrentId() {
        let editor = SubtitleEditor()
        editor.load(sampleEntries())
        editor.updateCurrent(atMs: 1500)
        #expect(editor.currentId == editor.entries[1].id)
    }

    @Test
    func updateCurrent_onlyFiresOnChange() {
        let editor = SubtitleEditor()
        editor.load(sampleEntries())
        editor.updateCurrent(atMs: 500)
        let id1 = editor.currentId
        // 同区间内多次调用，currentId 不变（节流效果）
        editor.updateCurrent(atMs: 600)
        editor.updateCurrent(atMs: 700)
        #expect(editor.currentId == id1)
        // 切换到下一条
        editor.updateCurrent(atMs: 1500)
        #expect(editor.currentId == editor.entries[1].id)
    }

    @Test
    func updateCurrent_outsideRange_setsNil() {
        let editor = SubtitleEditor()
        editor.load(sampleEntries())  // [0..3000]
        editor.updateCurrent(atMs: 9999)
        #expect(editor.currentId == nil)
    }

    // MARK: - exportEntries (feat-024 预留)

    @Test
    func exportEntries_roundtrip() {
        let editor = SubtitleEditor()
        let original = sampleEntries()
        editor.load(original)
        let exported = editor.exportEntries()
        #expect(exported.count == 3)
        #expect(exported[0].startMs == 0)
        #expect(exported[0].text == "你好")
        #expect(exported[1].endMs == 2000)
        #expect(exported[2].confidence == 0.8)
    }

    @Test
    func exportEntries_reflectsEdits() {
        let editor = SubtitleEditor()
        editor.load(sampleEntries())
        let id = editor.entries[0].id
        editor.updateText(at: id, text: "已修改")
        _ = editor.merge(at: 1)
        let exported = editor.exportEntries()
        #expect(exported.count == 2)
        #expect(exported[0].text == "已修改")
        #expect(exported[1].text == "世界 测试")
    }
}
