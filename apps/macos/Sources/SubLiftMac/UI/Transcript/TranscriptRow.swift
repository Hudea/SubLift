import SwiftUI

/// 10207：单条 Transcript 行。
///
/// 序号 + 等宽时间码 + 正文 + 必要的低置信警告；不使用逐行 Card 或常驻 confidence 数值。
/// 单击由 List selection 处理（seek）；Review 状态双击进入文本编辑；
/// 右键菜单提供 Edit/Split/Merge/Copy/Reveal（可用性按行与编辑权限派生）。
struct TranscriptRow: View {
    let entry: SubtitleEntry
    let index: Int
    let isCurrent: Bool
    let isEditable: Bool
    /// 行级命令可用性（由 TranscriptPanel 按 entries 与编辑权限派生）。
    let canSplit: Bool
    let canMerge: Bool
    let onTextChange: (String) -> Void
    let onSplit: () -> Void
    let onMerge: () -> Void
    let onCopy: () -> Void
    let onReveal: () -> Void

    @State private var isEditing = false

    var body: some View {
        HStack(alignment: .top, spacing: 8) {
            currentMarker

            Text("\(index)")
                .font(.system(.caption, design: .monospaced))
                .foregroundStyle(.tertiary)
                .frame(width: 26, alignment: .trailing)
                .accessibilityHidden(true)

            VStack(alignment: .leading, spacing: 4) {
                HStack(spacing: 8) {
                    Text(TranscriptRowPresentation.timeCodeText(for: entry))
                        .font(.system(.caption, design: .monospaced))
                        .foregroundStyle(.secondary)
                        .accessibilityLabel("时间 \(TimeFormatter.formatMs(entry.startMs)) 到 \(TimeFormatter.formatMs(entry.endMs))")

                    if TranscriptRowPresentation.needsLowConfidenceWarning(confidence: entry.confidence) {
                        Label("低置信", systemImage: "exclamationmark.triangle.fill")
                            .font(.caption2)
                            .foregroundStyle(.orange)
                            .accessibilityElement(children: .combine)
                            .accessibilityLabel("低置信度警告")
                    }

                    Spacer(minLength: 0)
                }

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
                            TapGesture(count: 2).onEnded {
                                if isEditable {
                                    isEditing = true
                                }
                            }
                        )
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 8)
        .contentShape(Rectangle())
        .contextMenu { contextMenuContent }
        .onChange(of: isEditable) { editable in
            if !editable {
                isEditing = false
            }
        }
    }

    // MARK: - Current 指示（Accent 2–3pt，与 selection 系统色分别表达）

    @ViewBuilder
    private var currentMarker: some View {
        if isCurrent {
            Capsule()
                .fill(Color.accentColor)
                .frame(width: 3, height: 34)
                .accessibilityLabel("当前播放字幕")
        } else {
            Color.clear
                .frame(width: 3, height: 34)
                .accessibilityHidden(true)
        }
    }

    // MARK: - 上下文菜单

    @ViewBuilder
    private var contextMenuContent: some View {
        Button("编辑") {
            if isEditable {
                isEditing = true
            }
        }
        .disabled(!isEditable)
        .accessibilityLabel("编辑字幕文本")

        Divider()

        Button("拆分", action: onSplit)
            .disabled(!canSplit)
        Button("与下一条合并", action: onMerge)
            .disabled(!canMerge)

        Divider()

        Button("复制文本", action: onCopy)
        Button("在播放头处显示", action: onReveal)
    }
}
