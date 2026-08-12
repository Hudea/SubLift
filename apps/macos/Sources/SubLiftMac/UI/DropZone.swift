import SwiftUI
import UniformTypeIdentifiers

/// Phase 10 V02：统一拖拽导入目标。
///
/// 接收整窗拖入的视频 URL，统一走 `onDrop` 导入 intent（与 Open Panel 同路径）。
/// - 拖拽激活时显示 Accent 边界（1.5–2 pt）与极浅 Accent tint（不依赖颜色表达状态；
///   动作文字由内容层在 `isTargeted` 时切换）。
/// - 不渲染永久虚线框；`isTargeted` 是瞬时拖拽状态，不进入 WorkspaceState。
struct DropTargetView<Content: View>: View {
    @Binding var isTargeted: Bool
    let onDrop: (URL) -> Bool
    @ViewBuilder let content: Content

    var body: some View {
        content
            .overlay {
                RoundedRectangle(cornerRadius: 12, style: .continuous)
                    .strokeBorder(
                        Color.accentColor,
                        lineWidth: isTargeted ? 2 : 0
                    )
                    .padding(4)
                    .allowsHitTesting(false)
                    .animation(.easeOut(duration: 0.12), value: isTargeted)
            }
            // tint 用 NSColor.withAlphaComponent 固定 alpha（动态 Color.opacity 在离屏渲染下不可靠），
            // 叠加在窗口背景色上，避免透明合成异常。
            .background(
                isTargeted
                    ? Color(nsColor: .controlAccentColor.withAlphaComponent(0.12))
                    : Color(nsColor: .windowBackgroundColor)
            )
            .dropDestination(
                for: URL.self,
                action: { items, _ in
                    guard let url = items.first else { return false }
                    return onDrop(url)
                },
                isTargeted: { targeted in
                    isTargeted = targeted
                }
            )
    }
}
