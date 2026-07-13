import CoreGraphics
import Foundation
import Testing
@testable import SubLiftMac

/// feat-022：坐标换算与区域合并纯函数单测。
struct RegionGeometryTests {

    @Test
    func visionNormalizedRect_convertsToTopLeftPixels() {
        // Vision 左下原点，高度 0.1、距底 0.2 → 顶部 y = (1 - 0.2 - 0.1) * 1080
        let normalized = CGRect(x: 0.25, y: 0.2, width: 0.5, height: 0.1)
        let rect = VideoCoordinateMapper.visionNormalizedRectToVideoPixels(
            normalized,
            videoWidth: 1920,
            videoHeight: 1080
        )
        #expect(rect.origin.x == 480)
        #expect(abs(rect.origin.y - 756) < 0.001)
        #expect(rect.size.width == 960)
        #expect(rect.size.height == 108)
    }

    @Test
    func aspectFitDisplayRect_letterboxesWideVideo() {
        let display = VideoCoordinateMapper.aspectFitDisplayRect(
            videoSize: CGSize(width: 1920, height: 1080),
            containerSize: CGSize(width: 640, height: 480)
        )
        #expect(display.width == 640)
        #expect(display.height == 360)
        #expect(display.origin.x == 0)
        #expect(display.origin.y == 60)
    }

    @Test
    func videoRectToViewRect_mapsPixelRectIntoDisplayRect() {
        let display = CGRect(x: 0, y: 60, width: 640, height: 360)
        let videoRect = CGRect(x: 0, y: 756, width: 1920, height: 108)
        let viewRect = VideoCoordinateMapper.videoRectToViewRect(
            videoRect,
            videoSize: CGSize(width: 1920, height: 1080),
            displayRect: display
        )
        #expect(viewRect.origin.x == 0)
        #expect(abs(viewRect.origin.y - 312) < 0.001)
        #expect(viewRect.size.width == 640)
        #expect(viewRect.size.height == 36)
    }

    @Test
    func mergedFullWidthRegion_usesSelectedYRangeAndFullWidth() {
        let candidates = [
            (id: 1, pixelRect: CGRect(x: 100, y: 800, width: 300, height: 40)),
            (id: 2, pixelRect: CGRect(x: 120, y: 860, width: 280, height: 36)),
            (id: 3, pixelRect: CGRect(x: 50, y: 120, width: 400, height: 30)),
        ]
        let merged = RegionMerger.mergedFullWidthRegion(
            candidates: candidates,
            selectedIds: [1, 2],
            videoWidth: 1920,
            videoHeight: 1080,
            paddingY: 8
        )
        #expect(merged != nil)
        #expect(merged?.origin.x == 0)
        #expect(merged?.size.width == 1920)
        #expect(merged?.minY == 792)
        #expect(merged?.maxY == 904)
    }

    @Test
    func mergedFullWidthRegion_returnsNilWhenNothingSelected() {
        let candidates = [(id: 1, pixelRect: CGRect(x: 0, y: 10, width: 10, height: 10))]
        let merged = RegionMerger.mergedFullWidthRegion(
            candidates: candidates,
            selectedIds: [],
            videoWidth: 1920,
            videoHeight: 1080
        )
        #expect(merged == nil)
    }

    @Test
    func autoSelectIds_prefersLowerBandCandidates() {
        let candidates: [(id: Int, pixelRect: CGRect, confidence: Float)] = [
            (id: 1, pixelRect: CGRect(x: 0, y: 100, width: 100, height: 30), confidence: 0.9),
            (id: 2, pixelRect: CGRect(x: 0, y: 900, width: 100, height: 30), confidence: 0.8),
            (id: 3, pixelRect: CGRect(x: 0, y: 950, width: 100, height: 30), confidence: 0.7),
        ]
        let ids = RegionMerger.autoSelectIds(candidates: candidates, videoHeight: 1080)
        #expect(ids == [2, 3])
    }

    @Test
    func autoSelectIds_returnsEmptyWhenNoLowerBand() {
        let candidates: [(id: Int, pixelRect: CGRect, confidence: Float)] = [
            (id: 1, pixelRect: CGRect(x: 0, y: 100, width: 100, height: 30), confidence: 0.9),
            (id: 2, pixelRect: CGRect(x: 0, y: 200, width: 100, height: 30), confidence: 0.8),
        ]
        let ids = RegionMerger.autoSelectIds(candidates: candidates, videoHeight: 1080)
        #expect(ids.isEmpty)
    }

    @Test
    func sampleTimestamps_spreadsAcrossDuration() {
        let ts = RegionFramePicker.sampleTimestamps(durationSeconds: 100)
        #expect(ts.count >= 5)
        #expect(ts.first! >= 10) // ~12%
        #expect(ts.last! <= 90) // ~88%
        // 单调递增
        for i in 1..<ts.count {
            #expect(ts[i] > ts[i - 1])
        }
    }

    @Test
    func sampleTimestamps_shortClipUsesFewerPoints() {
        let short = RegionFramePicker.sampleTimestamps(durationSeconds: 2)
        #expect(short.count == 2)
        #expect(short[0] < short[1])
    }

    @Test
    func sampleTimestamps_zeroDurationReturnsZero() {
        #expect(RegionFramePicker.sampleTimestamps(durationSeconds: 0) == [0])
    }

    @Test
    func subtitlePresenceScore_prefersLowerBandText() {
        let lowerOnly: [(pixelRect: CGRect, confidence: Float)] = [
            (CGRect(x: 100, y: 900, width: 400, height: 40), 0.9),
        ]
        let upperOnly: [(pixelRect: CGRect, confidence: Float)] = [
            (CGRect(x: 100, y: 80, width: 400, height: 40), 0.95),
        ]
        let empty: [(pixelRect: CGRect, confidence: Float)] = []
        let lowerScore = RegionFramePicker.subtitlePresenceScore(
            candidates: lowerOnly, videoHeight: 1080
        )
        let upperScore = RegionFramePicker.subtitlePresenceScore(
            candidates: upperOnly, videoHeight: 1080
        )
        let emptyScore = RegionFramePicker.subtitlePresenceScore(
            candidates: empty, videoHeight: 1080
        )
        #expect(lowerScore > 1.5)
        #expect(upperScore == 0)
        #expect(emptyScore == 0)
        #expect(lowerScore > upperScore)
    }

    @Test
    func subtitlePresenceScore_penalizesTooManyLowerBoxes() {
        let few: [(pixelRect: CGRect, confidence: Float)] = [
            (CGRect(x: 0, y: 900, width: 100, height: 30), 0.8),
            (CGRect(x: 0, y: 940, width: 100, height: 30), 0.8),
        ]
        let many: [(pixelRect: CGRect, confidence: Float)] = (0..<10).map { i in
            (CGRect(x: 0, y: 800 + CGFloat(i) * 20, width: 100, height: 18), Float(0.8))
        }
        let fewScore = RegionFramePicker.subtitlePresenceScore(candidates: few, videoHeight: 1080)
        let manyScore = RegionFramePicker.subtitlePresenceScore(candidates: many, videoHeight: 1080)
        // 1~3 框加权更高；10 框虽 conf 总和更大但 countFactor 惩罚后应更低或接近
        #expect(fewScore > 0)
        #expect(manyScore > 0)
        // 单框/双框字幕带应优于铺满下部的噪声
        #expect(fewScore >= manyScore * 0.5)
    }

    @Test
    func pickBestFrameIndex_selectsHighestScore() {
        #expect(RegionFramePicker.pickBestFrameIndex(scores: [0.1, 2.5, 1.0]) == 1)
        #expect(RegionFramePicker.pickBestFrameIndex(scores: [3.0, 2.0, 1.0]) == 0)
        #expect(RegionFramePicker.pickBestFrameIndex(scores: []) == nil)
        // 同分取先出现
        #expect(RegionFramePicker.pickBestFrameIndex(scores: [1.0, 1.0, 0.5]) == 0)
    }

    @Test
    func regionBoxFromMergedRegion_convertsToIntArray() {
        let rect = CGRect(x: 0, y: 792, width: 1920, height: 112)
        let box = RegionMerger.regionBoxFromMergedRegion(rect, videoWidth: 1920, videoHeight: 1080)
        #expect(box == [0, 792, 1920, 112])
    }

    @Test
    func regionBoxFromMergedRegion_clampsToVideoBounds() {
        let rect = CGRect(x: 0, y: 1070, width: 1920, height: 20)
        let box = RegionMerger.regionBoxFromMergedRegion(rect, videoWidth: 1920, videoHeight: 1080)
        #expect(box == [0, 1070, 1920, 10])
    }

    @Test
    func regionBoxFromMergedRegion_nilWhenNoRegion() {
        let box = RegionMerger.regionBoxFromMergedRegion(nil, videoWidth: 1920, videoHeight: 1080)
        #expect(box == nil)
    }

    @Test
    func regionBoxPalette_cyclesColors() {
        let first = RegionBoxPalette.color(for: 0)
        let ninth = RegionBoxPalette.color(for: 8)
        let wrapped = RegionBoxPalette.color(for: RegionBoxPalette.colors.count)
        #expect(first == RegionBoxPalette.colors[0])
        #expect(ninth == RegionBoxPalette.colors[0])
        #expect(wrapped == RegionBoxPalette.colors[0])
    }

    @Test
    func subtitleProfileFromSelection_mapsIntoCropCoords() {
        let candidates = [
            (
                id: 1,
                pixelRect: CGRect(x: 400, y: 820, width: 200, height: 40),
                textPreview: "中文字幕"
            ),
            (
                id: 2,
                pixelRect: CGRect(x: 50, y: 100, width: 100, height: 20),
                textPreview: "NEWS"
            ),
        ]
        // crop band at y=800 h=100 full width
        let regionBox: RegionBox = [0, 800, 1920, 100]
        let profile = RegionMerger.subtitleProfileFromSelection(
            candidates: candidates,
            selectedIds: [1],
            regionBox: regionBox
        )
        #expect(profile != nil)
        #expect(profile?.script == "cjk")
        #expect(profile?.centerX == 500)
        #expect(profile?.centerY == 40)
        #expect(profile?.height == 40)
        #expect(profile?.yMin == 20)
        #expect(profile?.yMax == 60)
    }

    @Test
    func subtitleProfileFromSelection_defaultsToFullCropWhenNoneSelected() {
        let regionBox: RegionBox = [0, 800, 1920, 100]
        let profile = RegionMerger.subtitleProfileFromSelection(
            candidates: [],
            selectedIds: [],
            regionBox: regionBox
        )
        #expect(profile?.centerX == 960)
        #expect(profile?.centerY == 50)
        #expect(profile?.height == 100)
        #expect(profile?.yMin == 0)
        #expect(profile?.yMax == 100)
        #expect(profile?.script == "auto")
    }

    @Test
    func subtitleProfileFromSelection_infersLatinAndMixedScripts() {
        let regionBox: RegionBox = [0, 800, 1920, 100]
        let candidates = [
            (
                id: 1,
                pixelRect: CGRect(x: 400, y: 820, width: 200, height: 40),
                textPreview: "HELLO"
            ),
            (
                id: 2,
                pixelRect: CGRect(x: 700, y: 820, width: 200, height: 40),
                textPreview: "欢迎 ZPD"
            ),
        ]

        let latin = RegionMerger.subtitleProfileFromSelection(
            candidates: candidates,
            selectedIds: [1],
            regionBox: regionBox
        )
        let mixed = RegionMerger.subtitleProfileFromSelection(
            candidates: candidates,
            selectedIds: [2],
            regionBox: regionBox
        )
        #expect(latin?.script == "latin")
        #expect(mixed?.script == "auto")
    }
}
