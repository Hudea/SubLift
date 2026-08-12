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

    /// 用户最后一次拖动选择的有效宽度；窗口/Inspector 改变时会再次按当前容器动态 clamp。
    /// 未发生拖动时使用父视图重算的 `initialVideoWidth`，开关 Inspector 会恢复目标比例。
    @State private var videoWidth: CGFloat?
    @State private var dragStartVideoWidth: CGFloat?

    var body: some View {
        GeometryReader { geometry in
            let container = geometry.size.width
            let columns = WorkspaceLayout.resolvedColumns(
                proposedLeftWidth: videoWidth ?? initialVideoWidth,
                containerWidth: container,
                dividerWidth: 1,
                leftMin: leftMin,
                rightMin: rightMin
            )
            HStack(spacing: 0) {
                left()
                    .frame(width: columns.left)

                Divider()
                    .frame(width: columns.divider)
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
                                let dragStart = dragStartVideoWidth ?? columns.left
                                if dragStartVideoWidth == nil {
                                    dragStartVideoWidth = dragStart
                                }
                                videoWidth = WorkspaceLayout.resolvedColumns(
                                    proposedLeftWidth: dragStart + value.translation.width,
                                    containerWidth: container,
                                    dividerWidth: columns.divider,
                                    leftMin: leftMin,
                                    rightMin: rightMin
                                ).left
                            }
                            .onEnded { _ in
                                dragStartVideoWidth = nil
                            }
                    )

                right()
                    .frame(width: columns.right)
            }
        }
    }
}
