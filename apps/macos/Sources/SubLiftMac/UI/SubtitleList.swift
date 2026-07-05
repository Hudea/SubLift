import SwiftUI

// MARK: - SubtitleList

/// feat-021：字幕列表 + 编辑器。
///
/// 功能：
/// - List + ForEach 展示字幕条目
/// - 双击 Text 进入 TextEditor 编辑
/// - 步进器（±100ms / ±10ms）调整 start/end
/// - 合并/拆分按钮
/// - 点击条目跳转视频；播放时高亮当前条目
struct SubtitleList: View {
    @ObservedObject var editor: SubtitleEditor
    let onSeek: (Int) -> Void
    let onExport: () -> Void

    var body: some View {
        VStack(spacing: 0) {
            header
            Divider()
            if editor.entries.isEmpty {
                emptyState
            } else {
                listContent
            }
        }
        .background(Color(nsColor: .textBackgroundColor))
    }

    // MARK: - Header

    private var header: some View {
        HStack(spacing: 8) {
            Text("字幕（\(editor.entries.count) 条）")
                .font(.headline)
            Spacer()
            Button {
                onExport()
            } label: {
                Label("导出 SRT", systemImage: "square.and.arrow.down")
            }
            .disabled(editor.entries.isEmpty)
            Button {
                if let id = editor.selectedId, let idx = editor.entries.firstIndex(where: { $0.id == id }) {
                    editor.split(at: idx)
                }
            } label: {
                Label("拆分", systemImage: "rectangle.split.2x1")
            }
            .disabled(editor.selectedId == nil)
            Button {
                if let id = editor.selectedId, let idx = editor.entries.firstIndex(where: { $0.id == id }) {
                    editor.merge(at: idx)
                }
            } label: {
                Label("合并", systemImage: "link")
            }
            .disabled(editor.selectedId == nil)
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 6)
    }

    // MARK: - Empty state

    private var emptyState: some View {
        VStack(spacing: 8) {
            Image(systemName: "captions.bubble")
                .font(.system(size: 40))
                .foregroundStyle(.secondary)
            Text("暂无字幕")
                .foregroundStyle(.secondary)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    // MARK: - List

    private var listContent: some View {
        List(selection: $editor.selectedId) {
            ForEach(editor.entries) { entry in
                SubtitleRow(
                    entry: entry,
                    isCurrent: entry.id == editor.currentId,
                    onTextChange: { newText in editor.updateText(at: entry.id, text: newText) }
                )
                .tag(entry.id)
                .listRowInsets(EdgeInsets())
            }
        }
        .onChange(of: editor.selectedId) { id in
            if let id, let entry = editor.entries.first(where: { $0.id == id }) {
                editor.updateCurrent(atMs: entry.startMs)
                onSeek(entry.startMs)
            }
        }
    }
}

// MARK: - SubtitleRow

/// 单条字幕行：时间码 + 可编辑文本。
private struct SubtitleRow: View {
    let entry: SubtitleEntry
    let isCurrent: Bool
    let onTextChange: (String) -> Void

    @State private var isEditing = false

    var body: some View {
        HStack(alignment: .top, spacing: 8) {
            currentMarker

            VStack(alignment: .leading, spacing: 4) {
                // 时间码
                HStack(spacing: 8) {
                    Text(TimeFormatter.formatMs(entry.startMs))
                        .font(.system(.caption, design: .monospaced))
                        .foregroundStyle(.secondary)
                    Text("→")
                        .font(.system(.caption, design: .monospaced))
                        .foregroundStyle(.secondary)
                    Text(TimeFormatter.formatMs(entry.endMs))
                        .font(.system(.caption, design: .monospaced))
                        .foregroundStyle(.secondary)
                    Spacer()
                }
                // 文本
                if isEditing {
                    TextEditor(text: Binding(
                        get: { entry.text },
                        set: { onTextChange($0) }
                    ))
                    .font(.body)
                    .frame(minHeight: 32)
                    .onExitCommand { isEditing = false }
                } else {
                    Text(entry.text.isEmpty ? "（空）" : entry.text)
                        .font(.body)
                        .foregroundStyle(entry.text.isEmpty ? .secondary : .primary)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .simultaneousGesture(
                            TapGesture(count: 2).onEnded { isEditing = true }
                        )
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(.horizontal, 10)
        .padding(.vertical, 8)
        .contentShape(Rectangle())
    }

    @ViewBuilder
    private var currentMarker: some View {
        if isCurrent {
            Capsule()
                .fill(Color(nsColor: .secondaryLabelColor))
                .frame(width: 3, height: 34)
                .accessibilityLabel("当前播放字幕")
        } else {
            Color.clear
                .frame(width: 3, height: 34)
                .accessibilityHidden(true)
        }
    }
}
