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
}

/// feat-033d：从用户选中的候选框推算 SubtitleProfile。
///
/// 把全帧坐标的选中候选框转换为裁剪图内（crop-relative）的 profile：
/// - y_center: 选中框中心 y 均值，减去 region_box 原点 y
/// - line_height: 选中框高度均值
/// - y_tolerance: 覆盖选中框中心 y 的实际跨度 + 单行抖动
///   （双行字幕两行中心相距约一个行高，lineHeight/2 不够）
/// - max_lines: 按选中框 y 分布聚类判定（简单版：唯一 y 个数，clamp 1...2）
enum SubtitleProfileBuilder {

    /// 从选中候选框 + region_box 生成 SubtitleProfile。
    ///
    /// - Parameters:
    ///   - selectedRects: 用户选中的候选框（全帧像素坐标）
    ///   - regionBox: region_box [x, y, width, height]（x 恒为 0，y 为裁剪原点）
    /// - Returns: crop-relative 的 SubtitleProfile；选中为空或 regionBox 无效时返回 nil
    static func fromSelection(
        selectedRects: [CGRect],
        regionBox: RegionBox?
    ) -> SubtitleProfile? {
        guard !selectedRects.isEmpty,
              let regionBox,
              regionBox.count == 4
        else { return nil }

        let cropOriginY = CGFloat(regionBox[1])

        let midYs = selectedRects.map { $0.midY }
        let centerYAvg = midYs.reduce(0, +) / CGFloat(midYs.count)
        let yCenter = Double(centerYAvg - cropOriginY)

        let heights = selectedRects.map { $0.height }
        let heightAvg = heights.reduce(0, +) / CGFloat(heights.count)
        let lineHeight = Double(heightAvg)

        let yTolerance = _computeYTolerance(midYs: midYs, centerYAvg: centerYAvg, lineHeight: lineHeight)

        let maxLines = _computeMaxLines(midYs: midYs, heightAvg: heightAvg)

        return SubtitleProfile(
            yCenter: yCenter,
            yTolerance: yTolerance,
            lineHeight: lineHeight,
            maxLines: maxLines,
            scriptHint: "auto"
        )
    }

    /// y_tolerance = 选中框中心 y 到均值的最大距离 + lineHeight/2（单行抖动余量）。
    /// 双行字幕两行中心相距约一个行高，均值居中，每行到均值距离 ≈ span/2，
    /// 加 lineHeight/2 余量保证两行都落在容差内。
    private static func _computeYTolerance(
        midYs: [CGFloat],
        centerYAvg: CGFloat,
        lineHeight: Double
    ) -> Double {
        let maxDist = midYs.map { abs($0 - centerYAvg) }.max() ?? 0
        return Double(maxDist) + lineHeight / 2.0
    }

    /// max_lines 按 y 中心聚类：相邻中心差 > heightAvg 视为不同行。
    private static func _computeMaxLines(midYs: [CGFloat], heightAvg: CGFloat) -> Int {
        let centers = midYs.sorted()
        var clusters: [CGFloat] = []
        for c in centers {
            if let last = clusters.last, abs(c - last) <= heightAvg {
                continue
            }
            clusters.append(c)
        }
        return max(1, min(2, clusters.count))
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