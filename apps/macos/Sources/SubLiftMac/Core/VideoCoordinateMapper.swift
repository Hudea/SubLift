import CoreGraphics
import Foundation

/// feat-022：视频像素坐标 ↔ 预览视图坐标换算（resizeAspect / fit）。
enum VideoCoordinateMapper {

    /// Vision 归一化框（原点左下）→ 视频像素框（原点左上）。
    static func visionNormalizedRectToVideoPixels(
        _ normalized: CGRect,
        videoWidth: Int,
        videoHeight: Int
    ) -> CGRect {
        let width = CGFloat(videoWidth)
        let height = CGFloat(videoHeight)
        let x = normalized.origin.x * width
        let y = (1.0 - normalized.origin.y - normalized.height) * height
        let w = normalized.width * width
        let h = normalized.height * height
        return CGRect(x: x, y: y, width: w, height: h)
    }

    /// 计算 resizeAspect fit 下视频在容器中的实际显示区域。
    static func aspectFitDisplayRect(videoSize: CGSize, containerSize: CGSize) -> CGRect {
        guard videoSize.width > 0, videoSize.height > 0,
              containerSize.width > 0, containerSize.height > 0 else {
            return .zero
        }

        let videoAspect = videoSize.width / videoSize.height
        let containerAspect = containerSize.width / containerSize.height

        if videoAspect > containerAspect {
            let displayWidth = containerSize.width
            let displayHeight = displayWidth / videoAspect
            let originY = (containerSize.height - displayHeight) / 2.0
            return CGRect(x: 0, y: originY, width: displayWidth, height: displayHeight)
        }

        let displayHeight = containerSize.height
        let displayWidth = displayHeight * videoAspect
        let originX = (containerSize.width - displayWidth) / 2.0
        return CGRect(x: originX, y: 0, width: displayWidth, height: displayHeight)
    }

    /// 视频像素矩形 → 预览视图矩形。
    static func videoRectToViewRect(
        _ videoRect: CGRect,
        videoSize: CGSize,
        displayRect: CGRect
    ) -> CGRect {
        guard videoSize.width > 0, videoSize.height > 0 else { return .zero }
        let scaleX = displayRect.width / videoSize.width
        let scaleY = displayRect.height / videoSize.height
        return CGRect(
            x: displayRect.origin.x + videoRect.origin.x * scaleX,
            y: displayRect.origin.y + videoRect.origin.y * scaleY,
            width: videoRect.width * scaleX,
            height: videoRect.height * scaleY
        )
    }
}

/// feat-022：由用户选中的候选框推算最终字幕带（X 全宽，Y 取选中框区间）。
enum RegionMerger {

    static let defaultPaddingY: CGFloat = 8
    static let defaultLowerBandRatio: CGFloat = 0.55

    /// 合并选中框为全宽字幕区域；无选中时返回 nil。
    static func mergedFullWidthRegion(
        candidates: [(id: Int, pixelRect: CGRect)],
        selectedIds: Set<Int>,
        videoWidth: Int,
        videoHeight: Int,
        paddingY: CGFloat = defaultPaddingY
    ) -> CGRect? {
        let selected = candidates.filter { selectedIds.contains($0.id) }
        guard !selected.isEmpty, videoWidth > 0, videoHeight > 0 else { return nil }

        let minY = selected.map { $0.pixelRect.minY }.min()! - paddingY
        let maxY = selected.map { $0.pixelRect.maxY }.max()! + paddingY
        let clampedMinY = max(0, minY)
        let clampedMaxY = min(CGFloat(videoHeight), maxY)
        guard clampedMaxY > clampedMinY else { return nil }

        return CGRect(
            x: 0,
            y: clampedMinY,
            width: CGFloat(videoWidth),
            height: clampedMaxY - clampedMinY
        )
    }

    /// 默认预选：中心 Y 落在画面下部区域的候选框。
    static func autoSelectIds(
        candidates: [(id: Int, pixelRect: CGRect, confidence: Float)],
        videoHeight: Int,
        lowerBandRatio: CGFloat = defaultLowerBandRatio,
        minimumConfidence: Float = 0.25
    ) -> Set<Int> {
        guard videoHeight > 0 else { return [] }
        let thresholdY = CGFloat(videoHeight) * lowerBandRatio
        let ids = candidates.compactMap { item -> Int? in
            let centerY = item.pixelRect.midY
            guard centerY >= thresholdY, item.confidence >= minimumConfidence else { return nil }
            return item.id
        }
        return Set(ids)
    }

    /// 合并区域 → IPC `region_box` `[x, y, width, height]`（整数、边界 clamp）。
    static func regionBoxFromMergedRegion(
        _ rect: CGRect?,
        videoWidth: Int,
        videoHeight: Int
    ) -> RegionBox? {
        guard let rect, videoWidth > 0, videoHeight > 0 else { return nil }

        let y = max(0, min(Int(rect.origin.y.rounded(.down)), videoHeight - 1))
        let maxHeight = videoHeight - y
        let height = max(1, min(Int(rect.height.rounded(.up)), maxHeight))
        let width = videoWidth

        return [0, y, width, height]
    }

    /// feat-034b：由用户选中框并集 + 全宽 crop，构造 crop 坐标系下的 `SubtitleProfilePayload`。
    ///
    /// - `regionBox`：全宽裁剪带 `[x,y,w,h]`（视频像素）
    /// - 选中框：真实字幕窄框（视频像素）；profile 几何相对 crop 原点
    static func subtitleProfileFromSelection(
        candidates: [(id: Int, pixelRect: CGRect)],
        selectedIds: Set<Int>,
        regionBox: RegionBox,
        script: String = "cjk"
    ) -> SubtitleProfilePayload? {
        guard regionBox.count == 4 else { return nil }
        let cropX = regionBox[0]
        let cropY = regionBox[1]
        let cropW = regionBox[2]
        let cropH = regionBox[3]
        guard cropW > 0, cropH > 0 else { return nil }

        let selected = candidates.filter { selectedIds.contains($0.id) }
        guard !selected.isEmpty else {
            // 无选中时用 crop 全带默认
            return SubtitleProfilePayload(
                script: script,
                centerX: cropW / 2,
                centerY: cropH / 2,
                height: cropH,
                yMin: 0,
                yMax: cropH
            )
        }

        let minX = selected.map { $0.pixelRect.minX }.min()!
        let maxX = selected.map { $0.pixelRect.maxX }.max()!
        let minY = selected.map { $0.pixelRect.minY }.min()!
        let maxY = selected.map { $0.pixelRect.maxY }.max()!

        // 视频像素 → crop 相对，再 clamp
        let relMinX = max(0, min(Int(minX.rounded(.down)) - cropX, cropW))
        let relMaxX = max(relMinX, min(Int(maxX.rounded(.up)) - cropX, cropW))
        let relMinY = max(0, min(Int(minY.rounded(.down)) - cropY, cropH))
        let relMaxY = max(relMinY, min(Int(maxY.rounded(.up)) - cropY, cropH))
        let bandW = max(0, relMaxX - relMinX)
        let bandH = max(0, relMaxY - relMinY)

        return SubtitleProfilePayload(
            script: script,
            centerX: relMinX + bandW / 2,
            centerY: relMinY + bandH / 2,
            height: bandH > 0 ? bandH : cropH,
            yMin: relMinY,
            yMax: relMaxY > relMinY ? relMaxY : cropH
        )
    }
}

/// feat-022：候选框配色（按编号区分）。
enum RegionBoxPalette {
    static let colors: [ColorComponents] = [
        ColorComponents(red: 0.95, green: 0.35, blue: 0.35),
        ColorComponents(red: 0.30, green: 0.75, blue: 0.95),
        ColorComponents(red: 0.98, green: 0.78, blue: 0.20),
        ColorComponents(red: 0.55, green: 0.85, blue: 0.45),
        ColorComponents(red: 0.85, green: 0.45, blue: 0.95),
        ColorComponents(red: 0.98, green: 0.55, blue: 0.20),
        ColorComponents(red: 0.40, green: 0.65, blue: 0.98),
        ColorComponents(red: 0.90, green: 0.40, blue: 0.65),
    ]

    struct ColorComponents: Equatable {
        let red: Double
        let green: Double
        let blue: Double
    }

    static func color(for index: Int) -> ColorComponents {
        let safe = max(0, index)
        return colors[safe % colors.count]
    }
}