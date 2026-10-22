import CoreGraphics
import Foundation
import Testing
@testable import SubLiftMac

/// Phase 21 D04：多帧候选聚合（RegionSelectionAggregator）纯逻辑单测。
///
/// 验证 RegionSelectionModel 的「多帧聚合 + band 决策」在不依赖 Vision/AVFoundation 时可测：
/// 自动选中排除水印行、band 驱动 mergedRegion、无锚点 fail-closed、与 RegionMerger 兼容。
struct RegionSelectionAggregationTests {

    private let videoWidth = 1920
    private let videoHeight = 1080

    // MARK: - 多帧聚合 + 水印排除

    @Test
    func multiFrameAggregationExcludesWatermarkRows() throws {
        var frames: [SampledFrameLines] = []
        for frame in 0..<4 {
            var lines: [SubtitleBandLine] = []
            if frame < 3 {
                lines.append(
                    normalizedLine(
                        frame: frame, x: 0.30, y: 0.79, width: 0.40, height: 0.06,
                        text: "第\(frame)句", confidence: 0.95
                    )
                )
            }
            lines.append(
                normalizedLine(
                    frame: frame, x: 0.02, y: 0.86, width: 0.12, height: 0.045,
                    text: "98Tang.COM", confidence: 0.9
                )
            )
            frames.append(SampledFrameLines(frameIndex: frame, lines: lines))
        }

        let aggregation = RegionSelectionAggregator.aggregate(
            frames: frames,
            representativeFrameIndex: 2,
            videoWidth: videoWidth,
            videoHeight: videoHeight
        )

        #expect(aggregation.detected)
        #expect(aggregation.selectedCandidateIds == [1])
        #expect(aggregation.diagnostics.anchorFrameIndex == 0)
        #expect(aggregation.excludedWatermarkCount >= 4)
        #expect(aggregation.diagnostics.sideWatermarkTrimmed)

        let merged = try #require(aggregation.mergedRegion)
        #expect(merged.minX > 0)
        #expect(merged.width < CGFloat(videoWidth))
        #expect(merged.minY > CGFloat(videoHeight) * 0.7)
        #expect(merged.maxY < CGFloat(videoHeight) * 0.98)

        let box = try #require(
            RegionSelectionIPCBox.make(merged, videoWidth: videoWidth, videoHeight: videoHeight)
        )
        // 归一化收窄端点为 0.2/0.8；浮点四舍五入允许 ±1px。
        #expect(box[0] > 0 && box[0] <= 384)
        #expect(box[2] < videoWidth && box[2] >= 1152)
        #expect(box[1] > 0)
        #expect(box[3] > 0)
    }

    @Test
    func aggregationKeepsLegacyRegionMergerCompatible() throws {
        let frames = [
            SampledFrameLines(
                frameIndex: 0,
                lines: [
                    pixelLine(frame: 0, x: 0.30, y: 0.79, width: 0.40, height: 0.06, text: "字幕"),
                ]
            )
        ]
        let aggregation = RegionSelectionAggregator.aggregate(
            frames: frames,
            representativeFrameIndex: 0,
            videoWidth: videoWidth,
            videoHeight: videoHeight
        )
        #expect(aggregation.detected)
        #expect(aggregation.selectedCandidateIds == [1])

        let representative = frames[0]
        let candidates = representative.lines.enumerated().map { index, line in
            (id: index + 1, pixelRect: line.pixelRect(videoWidth: videoWidth, videoHeight: videoHeight))
        }
        let legacyMerged = RegionMerger.mergedFullWidthRegion(
            candidates: candidates,
            selectedIds: aggregation.selectedCandidateIds,
            videoWidth: videoWidth,
            videoHeight: videoHeight
        )
        let legacy = try #require(legacyMerged)
        #expect(legacy.origin.x == 0)
        #expect(legacy.width == CGFloat(videoWidth))

        let legacyBox = RegionMerger.regionBoxFromMergedRegion(
            legacy, videoWidth: videoWidth, videoHeight: videoHeight
        )
        #expect(legacyBox?[0] == 0)
        #expect(legacyBox?[2] == videoWidth)
    }

    // MARK: - fail-closed

    @Test
    func urlOnlyAggregationFailsClosedWithoutWatermarkRegion() {
        var frames: [SampledFrameLines] = []
        for frame in 0..<4 {
            frames.append(
                SampledFrameLines(
                    frameIndex: frame,
                    lines: [
                        normalizedLine(
                            frame: frame, x: 0.35, y: 0.79, width: 0.30, height: 0.06,
                            text: "www.example.com", confidence: 0.9
                        ),
                    ]
                )
            )
        }
        let aggregation = RegionSelectionAggregator.aggregate(
            frames: frames,
            representativeFrameIndex: 0,
            videoWidth: videoWidth,
            videoHeight: videoHeight
        )
        #expect(!aggregation.detected)
        #expect(aggregation.mergedRegion == nil)
        #expect(aggregation.selectedCandidateIds.isEmpty)
        #expect(aggregation.band == nil)
        #expect(aggregation.diagnostics.reason == "no_subtitle_anchor")
        // 与 C++ 同口径：无锚点提前返回，排除计数保持 0（fail-closed 不返回水印框）。
        #expect(aggregation.excludedWatermarkCount == 0)
    }

    @Test
    func emptyFramesFailClosed() {
        let aggregation = RegionSelectionAggregator.aggregate(
            frames: [],
            representativeFrameIndex: 0,
            videoWidth: videoWidth,
            videoHeight: videoHeight
        )
        #expect(!aggregation.detected)
        #expect(aggregation.mergedRegion == nil)
        #expect(aggregation.selectedCandidateIds.isEmpty)
        #expect(aggregation.diagnostics.reason == "empty_input")
    }

    // MARK: - 代表帧候选语义

    @Test
    func representativeCandidatesOutsideBandStayUnselected() throws {
        var frames: [SampledFrameLines] = []
        for frame in 0..<3 {
            var lines: [SubtitleBandLine] = []
            if frame == 1 {
                lines.append(
                    normalizedLine(
                        frame: frame, x: 0.30, y: 0.05, width: 0.40, height: 0.06,
                        text: "TITLE", confidence: 0.99
                    )
                )
            }
            lines.append(
                normalizedLine(
                    frame: frame, x: 0.30, y: 0.79, width: 0.40, height: 0.06,
                    text: "字幕", confidence: 0.95
                )
            )
            frames.append(SampledFrameLines(frameIndex: frame, lines: lines))
        }

        let aggregation = RegionSelectionAggregator.aggregate(
            frames: frames,
            representativeFrameIndex: 1,
            videoWidth: videoWidth,
            videoHeight: videoHeight
        )
        #expect(aggregation.detected)
        // 代表帧第 1 行是 TITLE（band 外），第 2 行是字幕 → 仅 id 2 自动选中。
        #expect(aggregation.selectedCandidateIds == [2])
        let merged = try #require(aggregation.mergedRegion)
        #expect(merged.minY > CGFloat(videoHeight) * 0.7)
        #expect(merged.width == CGFloat(videoWidth))
    }

    // MARK: - 单帧模式

    @Test
    func singleFrameAggregationSelectsSubtitleCandidate() throws {
        let frames = [
            SampledFrameLines(
                frameIndex: 0,
                lines: [
                    normalizedLine(
                        frame: 0, x: 0.30, y: 0.79, width: 0.40, height: 0.06,
                        text: "当前帧字幕", confidence: 0.95
                    ),
                ]
            )
        ]
        let aggregation = RegionSelectionAggregator.aggregate(
            frames: frames,
            representativeFrameIndex: 0,
            videoWidth: videoWidth,
            videoHeight: videoHeight
        )
        #expect(aggregation.detected)
        #expect(aggregation.selectedCandidateIds == [1])
        let merged = try #require(aggregation.mergedRegion)
        #expect(merged.minY > CGFloat(videoHeight) * 0.7)
        #expect(merged.width == CGFloat(videoWidth))
    }

    // MARK: - RegionSelectionModel 可观察诊断（不触发 IO）

    @Test @MainActor
    func regionSelectionModelExposesBandDiagnosticsAndClears() {
        let model = RegionSelectionModel()
        #expect(model.bandDiagnostics == nil)
        #expect(model.excludedWatermarkCount == 0)
        #expect(model.autoSelectedIds.isEmpty)
        #expect(model.regionBoxForIPC() == nil)

        model.selectedIds = [1, 2]
        model.clear()
        #expect(model.selectedIds.isEmpty)
        #expect(model.candidates.isEmpty)
        #expect(model.mergedRegion == nil)
        #expect(model.bandDiagnostics == nil)
        #expect(model.excludedWatermarkCount == 0)
        #expect(model.autoSelectedIds.isEmpty)
    }

    // MARK: - 辅助

    private func normalizedLine(
        frame: Int,
        x: Double,
        y: Double,
        width: Double,
        height: Double,
        text: String,
        confidence: Double
    ) -> SubtitleBandLine {
        SubtitleBandLine(
            frameIndex: frame,
            normalizedRect: CGRect(x: x, y: y, width: width, height: height),
            text: text,
            confidence: Float(confidence)
        )
    }

    private func pixelLine(
        frame: Int,
        x: Double,
        y: Double,
        width: Double,
        height: Double,
        text: String,
        confidence: Double = 0.95
    ) -> SubtitleBandLine {
        SubtitleBandLine(
            frameIndex: frame,
            pixelRect: CGRect(
                x: x * Double(videoWidth),
                y: y * Double(videoHeight),
                width: width * Double(videoWidth),
                height: height * Double(videoHeight)
            ),
            text: text,
            confidence: Float(confidence),
            videoWidth: videoWidth,
            videoHeight: videoHeight
        )
    }
}
