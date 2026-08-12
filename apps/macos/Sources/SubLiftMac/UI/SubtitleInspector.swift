import SwiftUI

/// 10209：Subtitle Inspector 展示纯逻辑（只显示真实字段）。
enum SubtitleInspectorPresentation {

    struct Row: Equatable {
        let label: String
        let value: String
        let icon: String
    }

    /// 选中条目 → 真实字段行；无选中 → nil（显示空态提示）。
    static func rows(entry: SubtitleEntry?, index: Int?, total: Int) -> [Row]? {
        guard let entry, let index else { return nil }
        var result: [Row] = [
            Row(label: "索引", value: "\(index) / \(max(total, 1))", icon: "number"),
            Row(label: "时间", value: timeText(for: entry), icon: "clock"),
            Row(label: "文本", value: entry.text.isEmpty ? "（空）" : entry.text, icon: "text.alignleft"),
        ]
        if TranscriptRowPresentation.needsLowConfidenceWarning(confidence: entry.confidence) {
            result.append(
                Row(label: "警告", value: "低置信度（\(Int(entry.confidence * 100))%）", icon: "exclamationmark.triangle")
            )
        }
        return result
    }

    static func timeText(for entry: SubtitleEntry) -> String {
        "\(TimeFormatter.formatMs(entry.startMs)) → \(TimeFormatter.formatMs(entry.endMs))"
    }
}

/// 10209：Subtitle 模式 Inspector 内容。
///
/// 只显示选中字幕的真实字段（索引、时间、文本、低置信警告）；Review 状态允许
/// 在 Inspector 内编辑文本（经 editor.updateText）；processing 只读。
struct SubtitleInspector: View {
    @ObservedObject var editor: SubtitleEditor
    let accessMode: TranscriptAccessMode

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            if let selectedId = editor.selectedId,
               let index = editor.entries.firstIndex(where: { $0.id == selectedId }) {
                let entry = editor.entries[index]
                fieldsSection(entry: entry, index: index + 1)
                textEditorSection(entry: entry)
            } else {
                emptyState
            }
            Spacer(minLength: 0)
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 10)
        .accessibilityElement(children: .contain)
    }

    // MARK: - 字段区

    private func fieldsSection(entry: SubtitleEntry, index: Int) -> some View {
        let rows = SubtitleInspectorPresentation.rows(
            entry: entry,
            index: index,
            total: editor.entries.count
        ) ?? []
        return VStack(alignment: .leading, spacing: 0) {
            ForEach(Array(rows.enumerated()), id: \.offset) { _, row in
                HStack(alignment: .top, spacing: 8) {
                    Image(systemName: row.icon)
                        .font(.system(size: 12))
                        .foregroundStyle(.secondary)
                        .frame(width: 16)
                        .accessibilityHidden(true)
                    Text(row.label)
                        .font(.callout)
                        .foregroundStyle(.secondary)
                    Spacer(minLength: 4)
                    Text(row.value)
                        .font(.system(.callout, design: .monospaced))
                        .lineLimit(3)
                        .multilineTextAlignment(.trailing)
                }
                .padding(.vertical, 5)
                .accessibilityElement(children: .combine)
                .accessibilityLabel("\(row.label)：\(row.value)")
            }
        }
    }

    // MARK: - 文本编辑（Review 可编辑；processing 只读）

    private func textEditorSection(entry: SubtitleEntry) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            Text("文本")
                .font(.callout)
                .foregroundStyle(.secondary)
            TextEditor(text: Binding(
                get: { entry.text },
                set: { newText in
                    guard accessMode == .editable else { return }
                    editor.updateText(at: entry.id, text: newText)
                }
            ))
            .font(.body)
            .frame(minHeight: 72)
            .overlay(
                RoundedRectangle(cornerRadius: 4)
                    .stroke(Color(nsColor: .separatorColor), lineWidth: 1)
            )
            .disabled(accessMode != .editable)
            .accessibilityLabel("字幕文本")
        }
    }

    // MARK: - 空态

    private var emptyState: some View {
        VStack(spacing: 8) {
            Image(systemName: "text.bubble")
                .font(.system(size: 30))
                .foregroundStyle(.secondary)
            Text("在 Transcript 中选择一条字幕")
                .font(.callout)
                .foregroundStyle(.secondary)
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .accessibilityElement(children: .combine)
    }
}
