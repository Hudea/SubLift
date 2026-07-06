import SwiftUI
import UniformTypeIdentifiers

/// feat-020：拖拽导入组件。
///
/// 用 `.dropDestination(for: URL.self)` 接收拖入的视频文件 URL。
/// 整个窗口均可接收拖拽（包裹在最外层）。
struct DropZone<Content: View>: View {
    let onDrop: (URL) -> Void
    @ViewBuilder let content: Content

    var body: some View {
        content
            .dropDestination(for: URL.self) { items, _ in
                guard let url = items.first else { return false }
                onDrop(url)
                return true
            }
    }
}
