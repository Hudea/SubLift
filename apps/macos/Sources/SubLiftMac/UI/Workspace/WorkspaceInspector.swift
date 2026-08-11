import SwiftUI

/// 10104：Context Inspector 容器骨架（内容由 10105 起逐步填充）。
///
/// 本 Feature 只提供可开关、有确定宽度（`WorkspaceLayout.inspectorReservedWidth`）与
/// 关闭行为的容器；面板内仅显示模式标题与明确占位文案，**不展示任何假数据**。
/// 10105 将填充 Video metadata 等真实内容并完善模式切换。
struct WorkspaceInspector: View {
    let mode: InspectorMode
    let onClose: () -> Void

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 8) {
                Text(title)
                    .font(.headline)
                    .lineLimit(1)
                Spacer(minLength: 0)
                Button(action: onClose) {
                    Image(systemName: "xmark.circle.fill")
                        .foregroundStyle(.secondary)
                }
                .buttonStyle(.borderless)
                .help("关闭 Inspector")
                .accessibilityLabel("关闭 Inspector")
            }
            .padding(.horizontal, 12)
            .padding(.vertical, 8)

            Divider()

            Spacer(minLength: 0)

            VStack(spacing: 8) {
                Image(systemName: "sidebar.trailing")
                    .font(.system(size: 28))
                    .foregroundStyle(.tertiary)
                Text("详细内容将在 Inspector 升级中提供")
                    .font(.callout)
                    .foregroundStyle(.secondary)
                    .multilineTextAlignment(.center)
            }
            .padding(16)
            .accessibilityElement(children: .combine)

            Spacer(minLength: 0)
        }
        .frame(width: WorkspaceLayout.inspectorReservedWidth)
        .frame(maxHeight: .infinity)
        .background(Color(nsColor: .windowBackgroundColor))
        .accessibilityElement(children: .contain)
        .accessibilityLabel("Inspector")
    }

    private var title: String {
        switch mode {
        case .video: "视频信息"
        case .region: "字幕区域"
        case .extraction: "提取"
        case .subtitle: "字幕"
        }
    }
}
