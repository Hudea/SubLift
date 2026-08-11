import SwiftUI

/// 10104：可拖拽的 Video / Transcript Split。
///
/// HSplitView 的 idealWidth 对 flexible 子视图（GeometryReader 等）不生效，初始
/// 比例不可控；本组件用 HStack + DragGesture divider 实现：
/// - 初始比例精确由 `WorkspaceLayout.splitWidths` 决定（Video 60–65% / Transcript 35–40%）；
/// - 用户可拖动 divider，双侧最小宽度由合同约束（clamp 逻辑有单测）。
struct WorkspaceSplitView<Left: View, Right: View>: View {
    /// 初始左栏（Video）宽度。
    let initialVideoWidth: CGFloat
    let leftMin: CGFloat
    let rightMin: CGFloat
    @ViewBuilder let left: () -> Left
    @ViewBuilder let right: () -> Right

    @State private var videoWidth: CGFloat = 0

    var body: some View {
        GeometryReader { geometry in
            let container = geometry.size.width
            let resolved = resolvedVideoWidth(container: container)
            HStack(spacing: 0) {
                left()
                    .frame(width: resolved)

                Divider()
                    .frame(width: 1)
                    .overlay(
                        Rectangle()
                            .fill(Color(nsColor: .separatorColor))
                            .frame(width: 1)
                    )
                    .onHover { hovering in
                        if hovering {
                            NSCursor.resizeLeftRight.push()
                        } else {
                            NSCursor.pop()
                        }
                    }
                    .gesture(
                        DragGesture(minimumDistance: 1)
                            .onChanged { value in
                                videoWidth = WorkspaceLayout.clampedLeftWidth(
                                    proposed: initialVideoWidth + value.translation.width,
                                    containerWidth: container,
                                    leftMin: leftMin,
                                    rightMin: rightMin
                                )
                            }
                    )

                right()
                    .frame(width: max(container - resolved - 1, rightMin))
            }
            .onAppear {
                videoWidth = WorkspaceLayout.clampedLeftWidth(
                    proposed: initialVideoWidth,
                    containerWidth: container,
                    leftMin: leftMin,
                    rightMin: rightMin
                )
            }
        }
    }

    private func resolvedVideoWidth(container: CGFloat) -> CGFloat {
        guard videoWidth > 0 else {
            return WorkspaceLayout.clampedLeftWidth(
                proposed: initialVideoWidth,
                containerWidth: container,
                leftMin: leftMin,
                rightMin: rightMin
            )
        }
        return videoWidth
    }
}
