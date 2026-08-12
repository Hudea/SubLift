import SwiftUI

/// Welcome 的稳定布局契约：核心内容保持一个视觉组，隐私说明独立锚定底部。
enum WelcomeLayout {
    static let coreGroupSpacing: CGFloat = 24
    static let actionGroupSpacing: CGFloat = 14
    static let contentMaxWidth: CGFloat = 520

    static func verticalInset(forHeight height: CGFloat) -> CGFloat {
        height < 700 ? 24 : 40
    }
}

/// Phase 10 V01：Welcome / Empty 工作区。
///
/// 单一 Open 主动作 + 本机处理说明 + 支持格式；不使用整窗 Card、渐变或永久虚线框。
/// 拖拽激活时切换动作文字（V02 语义）；Accent 边界由外层 `DropTargetView` 提供。
struct WelcomeView: View {
    let onOpen: () -> Void
    let isDropTargeted: Bool

    var body: some View {
        GeometryReader { geometry in
            let verticalInset = WelcomeLayout.verticalInset(forHeight: geometry.size.height)

            ZStack {
                VStack(spacing: WelcomeLayout.coreGroupSpacing) {
                    VStack(spacing: 16) {
                        appIcon
                        Text("SubLift")
                            .font(.title.weight(.semibold))
                        Text("从视频中提取可编辑字幕")
                            .font(.subheadline)
                            .foregroundStyle(.secondary)
                    }
                    .accessibilityElement(children: .combine)

                    VStack(spacing: WelcomeLayout.actionGroupSpacing) {
                        Button(action: onOpen) {
                            Label("打开视频…", systemImage: "folder")
                                .font(.body)
                                .padding(.horizontal, 4)
                        }
                        .buttonStyle(.borderedProminent)
                        .controlSize(.large)
                        .accessibilityLabel("打开视频")
                        .accessibilityHint("从文件面板选择 MP4、MOV 或 MKV 视频")

                        Text(isDropTargeted ? "松开以打开视频" : "或将视频拖到此处")
                            .font(.callout)
                            .fontWeight(isDropTargeted ? .semibold : .regular)
                            .foregroundStyle(isDropTargeted ? Color.accentColor : Color.secondary)
                            .accessibilityLabel(
                                isDropTargeted
                                    ? "拖拽激活，松开以打开视频"
                                    : "或将视频拖到此处"
                            )

                        Text(VideoImportPolicy.supportedFormatsText)
                            .font(.caption)
                            .foregroundStyle(.tertiary)
                            .accessibilityLabel("支持格式：MP4、MOV、MKV")
                    }
                }
                .padding(.vertical, verticalInset)

                VStack {
                    Spacer(minLength: 0)
                    Label("所有处理均在本机完成", systemImage: "lock.shield")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .accessibilityLabel("所有处理均在本机完成")
                }
                .padding(.vertical, verticalInset)
            }
        }
        .frame(maxWidth: WelcomeLayout.contentMaxWidth)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    /// 装饰性应用图标（SF Symbol 替代参考图的品牌图形，不复制外部资产）。
    private var appIcon: some View {
        RoundedRectangle(cornerRadius: 18, style: .continuous)
            .fill(Color.accentColor)
            .frame(width: 84, height: 84)
            .overlay {
                Image(systemName: "captions.bubble.fill")
                    .font(.system(size: 36))
                    .foregroundStyle(.white)
            }
            .accessibilityHidden(true)
    }
}
