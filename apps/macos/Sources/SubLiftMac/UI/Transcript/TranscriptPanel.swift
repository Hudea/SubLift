import SwiftUI

// MARK: - 纯逻辑（10207）

/// Transcript 搜索过滤投影：只影响显示，不改变 entries 顺序或导出内容。
enum TranscriptSearch {

    /// 按 query 过滤 entries（大小写不敏感子串匹配；空白 query 返回全部）。
    static func filteredEntries(_ entries: [SubtitleEntry], query: String) -> [SubtitleEntry] {
        let trimmed = query.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return entries }
        return entries.filter { $0.text.localizedCaseInsensitiveContains(trimmed) }
    }
}

/// Transcript 行展示纯逻辑。
enum TranscriptRowPresentation {

    /// 低置信警告阈值（显式 UI 常量，测试锁定）。
    static let lowConfidenceThreshold = 0.6

    /// 低于阈值才显示低置信警告（不永久展示每行数值）。
    static func needsLowConfidenceWarning(confidence: Double) -> Bool {
        confidence < lowConfidenceThreshold
    }

    /// 等宽时间码文本："start → end"。
    static func timeCodeText(for entry: SubtitleEntry) -> String {
        "\(TimeFormatter.formatMs(entry.startMs)) → \(TimeFormatter.formatMs(entry.endMs))"
    }
}

/// 行级上下文命令可用性（按行索引与编辑权限派生）。
enum TranscriptRowAvailability {

    static func canSplit(
        entry: SubtitleEntry,
        in entries: [SubtitleEntry],
        accessMode: TranscriptAccessMode
    ) -> Bool {
        guard accessMode == .editable else { return false }
        guard entries.contains(where: { $0.id == entry.id }) else { return false }
        // 与 SubtitleEditor.split 语义一致：无内容不可拆。
        return !entry.text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
    }

    static func canMerge(
        entry: SubtitleEntry,
        in entries: [SubtitleEntry],
        accessMode: TranscriptAccessMode
    ) -> Bool {
        guard accessMode == .editable else { return false }
        guard let index = entries.firstIndex(where: { $0.id == entry.id }) else { return false }
        // 边界条目（最后一条）不能与下一条合并。
        return index < entries.count - 1
    }
}

// MARK: - TranscriptPanel

/// 10207：标准 Transcript Panel。
///
/// - Header 显示真实条数与搜索框；搜索只过滤显示，不改 entries 或导出。
/// - 行：序号 + 等宽时间码 + 正文 + 必要的低置信警告；不使用逐行 Card。
/// - selection（系统 List 选中）与 current（播放）分别表达。
/// - 单击 seek、Review 双击编辑；Edit/Split/Merge/Copy/Reveal 进右键菜单。
struct TranscriptPanel: View {
    @ObservedObject var editor: SubtitleEditor
    let onSeek: (Int) -> Void
    let onExport: () -> Void
    let accessMode: TranscriptAccessMode
    let commands: WorkspaceCommandAvailability
    /// 当前播放时间（ms），用于"在播放头处显示"。
    let currentMs: Int
    /// processing/finalizing 时显示的只读说明（VoiceOver 可读）。
    let readOnlyNotice: String?
    /// 选中字幕变化时通知（切换 Inspector 到 Subtitle 模式，不强制打开）。
    let onSelectSubtitle: (UUID?) -> Void

    @State private var searchQuery = ""
    /// 搜索变化后待滚动的当前播放字幕（List 可能因无结果被拆掉，重建后消费）。
    @State private var pendingScrollId: UUID?

    var body: some View {
        VStack(spacing: 0) {
            header
            if let readOnlyNotice {
                Label(readOnlyNotice, systemImage: "lock.fill")
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(.horizontal, 12)
                    .padding(.bottom, 6)
                    .accessibilityElement(children: .combine)
            }
            Divider()
            content
        }
        .background(Color(nsColor: .textBackgroundColor))
        .onChange(of: searchQuery) { _ in
            // 搜索变化（含清空）后恢复当前播放定位。
            pendingScrollId = editor.currentId
        }
    }

    // MARK: - Header（条数 + 搜索）

    private var header: some View {
        VStack(spacing: 8) {
            HStack(spacing: 8) {
                Text("字幕（\(editor.entries.count) 条）")
                    .font(.headline)
                    .accessibilityLabel("字幕，共 \(editor.entries.count) 条")
                Spacer()
            }

            HStack(spacing: 6) {
                Image(systemName: "magnifyingglass")
                    .foregroundStyle(.secondary)
                TextField("搜索字幕…", text: $searchQuery)
                    .textFieldStyle(.roundedBorder)
                    .font(.callout)
                    .accessibilityLabel("搜索字幕")
                if !searchQuery.isEmpty {
                    Button {
                        searchQuery = ""
                    } label: {
                        Image(systemName: "xmark.circle.fill")
                            .foregroundStyle(.secondary)
                    }
                    .buttonStyle(.borderless)
                    .help("清除搜索")
                    .accessibilityLabel("清除搜索")
                }
            }
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
    }

    // MARK: - Content

    @ViewBuilder
    private var content: some View {
        let filtered = TranscriptSearch.filteredEntries(editor.entries, query: searchQuery)

        if editor.entries.isEmpty {
            emptyState(icon: "captions.bubble", title: "尚未提取字幕", detail: nil)
        } else if filtered.isEmpty {
            emptyState(icon: "magnifyingglass", title: "没有匹配的字幕", detail: "尝试其他关键词，或清除搜索。")
        } else {
            listContent(filtered)
        }
    }

    private func emptyState(icon: String, title: String, detail: String?) -> some View {
        VStack(spacing: 8) {
            Image(systemName: icon)
                .font(.system(size: 36))
                .foregroundStyle(.secondary)
            Text(title)
                .font(.callout)
                .foregroundStyle(.secondary)
            if let detail {
                Text(detail)
                    .font(.caption)
                    .foregroundStyle(.tertiary)
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .accessibilityElement(children: .combine)
    }

    private func listContent(_ filtered: [SubtitleEntry]) -> some View {
        ScrollViewReader { proxy in
            List(selection: $editor.selectedId) {
                ForEach(Array(filtered.enumerated()), id: \.element.id) { index, entry in
                    TranscriptRow(
                        entry: entry,
                        index: index + 1,
                        isCurrent: entry.id == editor.currentId,
                        isEditable: accessMode == .editable,
                        canSplit: commands.canSplit && TranscriptRowAvailability.canSplit(
                            entry: entry, in: editor.entries, accessMode: accessMode
                        ),
                        canMerge: commands.canMerge && TranscriptRowAvailability.canMerge(
                            entry: entry, in: editor.entries, accessMode: accessMode
                        ),
                        onTextChange: { newText in
                            guard accessMode == .editable else { return }
                            editor.updateText(at: entry.id, text: newText)
                        },
                        onSplit: {
                            if let idx = editor.entries.firstIndex(where: { $0.id == entry.id }) {
                                editor.split(at: idx)
                            }
                        },
                        onMerge: {
                            if let idx = editor.entries.firstIndex(where: { $0.id == entry.id }) {
                                editor.merge(at: idx)
                            }
                        },
                        onCopy: {
                            NSPasteboard.general.clearContents()
                            NSPasteboard.general.setString(entry.text, forType: .string)
                        },
                        onReveal: {
                            // 在播放头处显示：只滚动定位，不改变播放位置或选中。
                            if let current = editor.currentEntry(atMs: currentMs) {
                                withAnimation {
                                    proxy.scrollTo(current.id, anchor: .center)
                                }
                            }
                        }
                    )
                    .id(entry.id)
                    .tag(entry.id)
                    .listRowInsets(EdgeInsets())
                }
            }
            .onChange(of: editor.selectedId) { id in
                onSelectSubtitle(id)
                if let id, let entry = editor.entries.first(where: { $0.id == id }) {
                    editor.updateCurrent(atMs: entry.startMs)
                    onSeek(entry.startMs)
                }
            }
            .onChange(of: editor.currentId) { id in
                if let id {
                    withAnimation {
                        proxy.scrollTo(id, anchor: .center)
                    }
                }
            }
            .onChange(of: searchQuery) { _ in
                // 保持旧路径：List 存在时直接恢复定位（pendingScrollId 由顶层 onChange 维护）。
                if let currentId = editor.currentId {
                    withAnimation {
                        proxy.scrollTo(currentId, anchor: .center)
                    }
                }
            }
            .onChange(of: pendingScrollId) { id in
                if let id {
                    withAnimation {
                        proxy.scrollTo(id, anchor: .center)
                    }
                    pendingScrollId = nil
                }
            }
            .onAppear {
                // List 重建（搜索无结果→清除）后消费待滚动定位。
                if let id = pendingScrollId {
                    withAnimation {
                        proxy.scrollTo(id, anchor: .center)
                    }
                    pendingScrollId = nil
                }
            }
        }
    }
}
