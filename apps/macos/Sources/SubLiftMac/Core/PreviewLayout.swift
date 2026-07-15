import CoreGraphics

/// 左侧视频预览的尺寸策略。
///
/// 预览优先保持源视频比例，但必须给播放控制、字幕区域和提取栏留下稳定空间，
/// 防止预览在小窗口中挤压下方交互。
enum PreviewLayout {
    static let fallbackAspectRatio: CGFloat = 16.0 / 9.0
    static let maximumHeight: CGFloat = 420
    static let reservedControlsHeight: CGFloat = 240

    static func height(containerSize: CGSize, videoSize: CGSize) -> CGFloat {
        let aspectRatio: CGFloat
        if videoSize.width > 0, videoSize.height > 0 {
            aspectRatio = videoSize.width / videoSize.height
        } else {
            aspectRatio = fallbackAspectRatio
        }

        let aspectFitHeight = containerSize.width / aspectRatio
        let availableHeight = max(0, containerSize.height - reservedControlsHeight)
        return min(aspectFitHeight, availableHeight, maximumHeight)
    }
}
