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

    // MARK: - SubtitleProfileBuilder (feat-033d)

    @Test
    func profileBuilder_singleLine() {
        let rect = CGRect(x: 100, y: 940, width: 300, height: 50)
        let regionBox: RegionBox = [0, 900, 1920, 180]
        let profile = SubtitleProfileBuilder.fromSelection(
            selectedRects: [rect],
            regionBox: regionBox
        )
        #expect(profile != nil)
        // 中心 y = 940 + 25 = 965，减去 crop 原点 900 = 65
        #expect(profile?.yCenter == 65.0)
        #expect(profile?.lineHeight == 50.0)
        #expect(profile?.maxLines == 1)
        // 单行：maxDist=0, yTolerance = 0 + 50/2 = 25
        #expect(profile?.yTolerance == 25.0)
    }

    @Test
    func profileBuilder_doubleLine() {
        let r1 = CGRect(x: 100, y: 920, width: 300, height: 45)
        let r2 = CGRect(x: 100, y: 985, width: 300, height: 45)
        let regionBox: RegionBox = [0, 900, 1920, 180]
        let profile = SubtitleProfileBuilder.fromSelection(
            selectedRects: [r1, r2],
            regionBox: regionBox
        )
        #expect(profile != nil)
        #expect(profile?.maxLines == 2)
        // r1.midY = 942.5, r2.midY = 1007.5, 均值 = 975, 减去 crop 原点 900 = 75
        #expect(profile?.yCenter == 75.0)
        // 双行：maxDist = 32.5, yTolerance = 32.5 + 45/2 = 55.0
        // 两行中心到均值距离都是 32.5，加 lineHeight/2 余量保证两行都落在容差内
        #expect(profile?.yTolerance == 55.0)
    }

    @Test
    func profileBuilder_doubleLine_selects_both_lines() {
        /// 端到端验证：用 Swift builder 对双行字幕的 profile 输出，selector 应同时选中两行。
        /// 之前 yTolerance = lineHeight/2 时会漏选双行，见缺陷报告。
        let r1 = CGRect(x: 100, y: 920, width: 300, height: 45)
        let r2 = CGRect(x: 100, y: 985, width: 300, height: 45)
        let regionBox: RegionBox = [0, 900, 1920, 180]
        let profile = SubtitleProfileBuilder.fromSelection(
            selectedRects: [r1, r2],
            regionBox: regionBox
        )
        #expect(profile != nil)

        // 模拟 Python selector 的 y 轨道过滤逻辑
        // crop-relative: r1.y=20 h=45 (center 42.5), r2.y=85 h=45 (center 107.5)
        let yCenter = profile!.yCenter
        let yTolerance = profile!.yTolerance
        let center1 = 942.5 - 900  // 42.5
        let center2 = 1007.5 - 900  // 107.5
        #expect(abs(center1 - yCenter) <= yTolerance)
        #expect(abs(center2 - yCenter) <= yTolerance)
    }

    @Test
    func profileBuilder_nilWhenEmpty() {
        let regionBox: RegionBox = [0, 900, 1920, 180]
        let profile = SubtitleProfileBuilder.fromSelection(
            selectedRects: [],
            regionBox: regionBox
        )
        #expect(profile == nil)
    }

    @Test
    func profileBuilder_nilWhenNoRegionBox() {
        let rect = CGRect(x: 100, y: 940, width: 300, height: 50)
        let profile = SubtitleProfileBuilder.fromSelection(
            selectedRects: [rect],
            regionBox: nil
        )
        #expect(profile == nil)
    }
}