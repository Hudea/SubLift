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

/// 多时间点抽帧：挑选更可能含字幕的代表帧（避免单帧落在无字幕画面）。
enum RegionFramePicker {

    /// 默认在片长上的采样比例（避开片头/片尾纯黑或片尾字幕）。
    static let defaultSampleRatios: [Double] = [0.12, 0.25, 0.38, 0.5, 0.62, 0.75, 0.88]

    /// 下部字幕带得分达到该值时可提前停止后续抽帧（性能）。
    static let earlyStopScore: Double = 1.8

    /// 根据片长生成采样时间点（秒），去重并排序。
    static func sampleTimestamps(
        durationSeconds: Double,
        ratios: [Double] = defaultSampleRatios,
        maxSamples: Int = 7
    ) -> [Double] {
        guard durationSeconds.isFinite, durationSeconds > 0 else { return [0] }

        let capped = max(1, maxSamples)
        let picked = Array(ratios.prefix(capped))
        // 极短片：至少中点 + 靠后一点
        let effectiveRatios: [Double]
        if durationSeconds < 3 {
            effectiveRatios = [0.4, 0.7]
        } else if durationSeconds < 8 {
            effectiveRatios = [0.2, 0.45, 0.7]
        } else {
            effectiveRatios = picked.isEmpty ? [0.5] : picked
        }

        let endPad = min(0.05, durationSeconds * 0.01)
        var seconds = effectiveRatios.map { ratio in
            max(0, min(durationSeconds * ratio, max(0, durationSeconds - endPad)))
        }

        // 毫秒级去重，保持稳定顺序
        var seen = Set<Int>()
        seconds = seconds.filter { t in
            let key = Int((t * 1000).rounded())
            return seen.insert(key).inserted
        }
        return seconds
    }

    /// 评估一帧检测结果是否像「有底部字幕」。分数越高越优先作代表帧。
    ///
    /// 只统计画面下部（`lowerBandRatio` 以下）且置信度达标的框；
    /// 1~3 个下部框加权最高（典型硬字幕行数）。
    static func subtitlePresenceScore(
        candidates: [(pixelRect: CGRect, confidence: Float)],
        videoHeight: Int,
        lowerBandRatio: CGFloat = RegionMerger.defaultLowerBandRatio,
        minimumConfidence: Float = 0.25
    ) -> Double {
        guard videoHeight > 0, !candidates.isEmpty else { return 0 }

        let thresholdY = CGFloat(videoHeight) * lowerBandRatio
        let lower = candidates.filter {
            $0.pixelRect.midY >= thresholdY && $0.confidence >= minimumConfidence
        }
        guard !lower.isEmpty else { return 0 }

        let confSum = lower.reduce(0.0) { $0 + Double($1.confidence) }
        let countFactor: Double
        switch lower.count {
        case 1...3: countFactor = 2.0
        case 4...6: countFactor = 1.0
        default: countFactor = 0.35
        }
        // 轻微奖励：框越多但已在 countFactor 惩罚噪声；再加一点置信度底分
        return confSum * countFactor + Double(lower.count) * 0.15
    }

    /// 在多帧候选中选得分最高的一帧；同分取先出现（更靠前的采样点）。
    static func pickBestFrameIndex(scores: [Double]) -> Int? {
        guard !scores.isEmpty else { return nil }
        var bestIndex = 0
        var bestScore = scores[0]
        for i in 1..<scores.count {
            if scores[i] > bestScore {
                bestScore = scores[i]
                bestIndex = i
            }
        }
        return bestIndex
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
    /// 无下部候选时返回空集（不再全选，避免把整屏文字并成巨大 region）。
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
        candidates: [(id: Int, pixelRect: CGRect, textPreview: String)],
        selectedIds: Set<Int>,
        regionBox: RegionBox,
        script: String? = nil
    ) -> SubtitleProfilePayload? {
        guard regionBox.count == 4 else { return nil }
        let cropX = regionBox[0]
        let cropY = regionBox[1]
        let cropW = regionBox[2]
        let cropH = regionBox[3]
        guard cropW > 0, cropH > 0 else { return nil }

        let selected = candidates.filter { selectedIds.contains($0.id) }
        let effectiveScript = script ?? inferScript(
            from: selected.map(\.textPreview)
        )
        guard !selected.isEmpty else {
            // 无选中时用 crop 全带默认
            return SubtitleProfilePayload(
                script: effectiveScript,
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
            script: effectiveScript,
            centerX: relMinX + bandW / 2,
            centerY: relMinY + bandH / 2,
            height: bandH > 0 ? bandH : cropH,
            yMin: relMinY,
            yMax: relMaxY > relMinY ? relMaxY : cropH
        )
    }

    private static func inferScript(from texts: [String]) -> String {
        var hasCJK = false
        var hasLatin = false

        for scalar in texts.joined().unicodeScalars {
            switch scalar.value {
            case 0x3400...0x4DBF, 0x4E00...0x9FFF, 0xF900...0xFAFF:
                hasCJK = true
            case 0x41...0x5A, 0x61...0x7A:
                hasLatin = true
            default:
                continue
            }
        }

        if hasCJK && !hasLatin { return "cjk" }
        if hasLatin && !hasCJK { return "latin" }
        return "auto"
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
