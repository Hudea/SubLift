import CoreGraphics
import Foundation
import Testing
@testable import SubLiftMac

/// Phase 21 D04：SubtitleBandDetector 纯逻辑单测。
///
/// 场景与 C++ D02 的 cpp/tests/subtitle_band_test.cpp 对齐（同口径）；另用 D01 录制
/// fixture 做黄金校验，确保 macOS 与 server 端条带决策一致。
struct SubtitleBandDetectorTests {

    // MARK: - URL / 域名水印标记

    @Test
    func watermarkDomainDetectionIsConservative() {
        #expect(SubtitleBandDetector.containsWatermarkDomain("98Tang.COM"))
        #expect(SubtitleBandDetector.containsWatermarkDomain("暖暖的猫 98Tang.COM"))
        #expect(SubtitleBandDetector.containsWatermarkDomain("www.example.com"))
        #expect(SubtitleBandDetector.containsWatermarkDomain("https://example.com/x"))
        #expect(!SubtitleBandDetector.containsWatermarkDomain("你好，世界"))
        #expect(!SubtitleBandDetector.containsWatermarkDomain("ZPD 警局"))
        #expect(!SubtitleBandDetector.containsWatermarkDomain("等等..."))
        #expect(!SubtitleBandDetector.containsWatermarkDomain("3.14"))
        #expect(!SubtitleBandDetector.containsWatermarkDomain("No.1"))
    }

    // MARK: - 单行 / 双行字幕

    @Test
    func singleCenteredSubtitleLineProducesTightAdaptiveBand() {
        var lines: [SubtitleBandLine] = []
        for frame in 0..<8 where frame != 3 && frame != 7 {
            lines.append(makeLine(frame, 0.30, 0.79, 0.40, 0.06, "第\(frame)句"))
        }
        var config = SubtitleBandConfig.standard
        config.sampleFramesHint = 8  // 与 C++ region_detector 同口径：传实际采样帧数
        let result = SubtitleBandDetector.detect(lines: lines, config: config)
        #expect(result.detected)
        #expect(result.x == 0.0)
        #expect(result.width == 1.0)
        #expect(result.y > 0.75)
        #expect(result.y < 0.80)
        #expect(result.height > 0.07)
        #expect(result.height < 0.12)
        #expect(result.diagnostics.anchorSupportFrames == 6)
        #expect(result.diagnostics.sampleFrames == 8)
        #expect(result.previewText.contains("第"))
        #expect(result.diagnostics.reason.isEmpty)
    }

    @Test
    func sampleFramesDerivedFromFrameIndexWhenNoHint() {
        var lines: [SubtitleBandLine] = []
        for frame in 0..<8 where frame != 3 && frame != 7 {
            lines.append(makeLine(frame, 0.30, 0.79, 0.40, 0.06, "第\(frame)句"))
        }
        // sampleFramesHint=0（默认）时由 max(frameIndex)+1 推导；缺 7 号帧 → 7。
        let result = SubtitleBandDetector.detect(lines: lines)
        #expect(result.diagnostics.sampleFrames == 7)
    }

    @Test
    func sampleFramesHintOverridesDerivedCount() {
        var lines: [SubtitleBandLine] = []
        for frame in [0, 2, 4] {
            lines.append(makeLine(frame, 0.30, 0.79, 0.40, 0.06, "第\(frame)句"))
        }
        var config = SubtitleBandConfig.standard
        config.sampleFramesHint = 3
        let result = SubtitleBandDetector.detect(lines: lines, config: config)
        #expect(result.detected)
        #expect(result.diagnostics.sampleFrames == 3)
        #expect(result.diagnostics.anchorSupportFrames == 3)
    }

    @Test
    func multiLineSubtitleExpandsBandAcrossLineGap() {
        var lines: [SubtitleBandLine] = []
        for frame in 0..<6 {
            lines.append(makeLine(frame, 0.30, 0.76, 0.40, 0.06, "上句"))
            lines.append(makeLine(frame, 0.32, 0.83, 0.36, 0.06, "下句"))
        }
        let result = SubtitleBandDetector.detect(lines: lines)
        #expect(result.detected)
        #expect(result.y < 0.76)
        #expect(result.y + result.height > 0.89)
        #expect(result.height < 0.22)
        #expect(result.diagnostics.includedLines >= 6)
    }

    // MARK: - 水印

    @Test
    func sidePersistentWatermarkIsExcludedAndXTrimsSafely() {
        var lines: [SubtitleBandLine] = []
        for frame in 0..<8 {
            if frame < 6 {
                lines.append(makeLine(frame, 0.30, 0.79, 0.40, 0.06, "正常字幕"))
            }
            lines.append(makeLine(frame, 0.02, 0.86, 0.12, 0.045, "98Tang.COM", confidence: 0.9))
            lines.append(makeLine(frame, 0.03, 0.90, 0.10, 0.045, "暖暖的猫", confidence: 0.9))
        }
        let result = SubtitleBandDetector.detect(lines: lines)
        #expect(result.detected)
        #expect(result.diagnostics.excludedWatermarkLines >= 8)
        #expect(result.diagnostics.sideWatermarkTrimmed)
        #expect(result.x > 0.15)
        #expect(result.x + result.width < 0.85)
        #expect(result.width <= 0.65)
        #expect(result.previewText.contains("正常字幕"))
        #expect(!result.previewText.contains("98Tang"))
        #expect(result.width >= 0.60)
    }

    @Test
    func scrollingURLWatermarkDoesNotBecomeTheAnchor() {
        var lines: [SubtitleBandLine] = []
        let watermarkX: [Double] = [0.10, 0.30, 0.50, 0.70, 0.20, 0.60, 0.40, 0.80]
        for frame in 0..<8 {
            if frame < 6 {
                lines.append(makeLine(frame, 0.30, 0.79, 0.40, 0.06, "第\(frame)句台词"))
            }
            lines.append(
                makeLine(
                    frame, watermarkX[frame], 0.80, 0.20, 0.045,
                    "暖暖的猫 98Tang.COM", confidence: 0.9
                )
            )
        }
        let result = SubtitleBandDetector.detect(lines: lines)
        #expect(result.detected)
        #expect(result.diagnostics.anchorFrameIndex >= 0)
        #expect(result.diagnostics.anchorLineHeight > 0.05)
        #expect(!result.previewText.contains("98Tang"))
        #expect(result.diagnostics.excludedWatermarkLines >= 1)
        #expect(result.y < 0.82)
    }

    @Test
    func oversizedLogoBoxIsRejected() {
        var lines: [SubtitleBandLine] = []
        for frame in 0..<8 {
            lines.append(makeLine(frame, 0.40, 0.55, 0.20, 0.28, "DisNep", confidence: 0.99))
            if frame < 6 {
                lines.append(makeLine(frame, 0.30, 0.79, 0.40, 0.06, "字幕内容"))
            }
        }
        let result = SubtitleBandDetector.detect(lines: lines)
        #expect(result.detected)
        #expect(result.y > 0.72)
        #expect(result.height < 0.20)
        #expect(!result.previewText.contains("DisNep"))
    }

    // MARK: - fail-closed / 单帧 / letterbox

    @Test
    func emptyInputFailsClosed() {
        let result = SubtitleBandDetector.detect(lines: [])
        #expect(!result.detected)
        #expect(result.band == nil)
        #expect(result.diagnostics.reason == "empty_input")
        #expect(result.x == 0.0)
        #expect(result.width == 1.0)
    }

    @Test
    func onlyURLLinesFailClosedWithoutReturningWatermarkBox() {
        var lines: [SubtitleBandLine] = []
        for frame in 0..<8 {
            lines.append(makeLine(frame, 0.35, 0.79, 0.30, 0.06, "www.example.com", confidence: 0.9))
        }
        let result = SubtitleBandDetector.detect(lines: lines)
        #expect(!result.detected)
        #expect(result.band == nil)
        #expect(result.diagnostics.reason == "no_subtitle_anchor")
        #expect(result.x == 0.0)
        #expect(result.width == 1.0)
        // 与 C++ 同口径：无锚点提前返回，不做排除行统计（排除计数只在检出 band 时填充）。
        #expect(result.diagnostics.excludedWatermarkLines == 0)
    }

    @Test
    func singleFramePlayheadModeDetectsOneLine() {
        let lines = [makeLine(0, 0.30, 0.79, 0.40, 0.06, "当前帧字幕")]
        let result = SubtitleBandDetector.detect(lines: lines)
        #expect(result.detected)
        #expect(result.diagnostics.sampleFrames == 1)
        #expect(result.diagnostics.anchorSupportFrames == 1)
        #expect(result.y > 0.74)
        #expect(result.y < 0.80)
        #expect(result.width == 1.0)
    }

    @Test
    func letterboxedLowerSubtitleStillDetected() {
        var lines: [SubtitleBandLine] = []
        for frame in 0..<6 {
            lines.append(makeLine(frame, 0.32, 0.86, 0.36, 0.05, "字母框字幕"))
        }
        let result = SubtitleBandDetector.detect(lines: lines)
        #expect(result.detected)
        #expect(result.y > 0.82)
        #expect(result.y + result.height <= 1.0)
        #expect(result.x == 0.0)
        #expect(result.width == 1.0)
    }

    // MARK: - 像素 / 归一化输入等价

    @Test
    func pixelRectInputMatchesNormalizedRectInput() {
        let videoWidth = 1920
        let videoHeight = 1080
        let pixelRect = CGRect(
            x: 0.30 * 1920,
            y: 0.79 * 1080,
            width: 0.40 * 1920,
            height: 0.06 * 1080
        )
        var pixelLines: [SubtitleBandLine] = []
        var normalizedLines: [SubtitleBandLine] = []
        for frame in 0..<6 {
            pixelLines.append(
                SubtitleBandLine(
                    frameIndex: frame,
                    pixelRect: pixelRect,
                    text: "字幕",
                    confidence: 0.95,
                    videoWidth: videoWidth,
                    videoHeight: videoHeight
                )
            )
            normalizedLines.append(
                SubtitleBandLine(
                    frameIndex: frame,
                    normalizedRect: CGRect(x: 0.30, y: 0.79, width: 0.40, height: 0.06),
                    text: "字幕",
                    confidence: 0.95
                )
            )
        }
        let fromPixels = SubtitleBandDetector.detect(lines: pixelLines)
        let fromNormalized = SubtitleBandDetector.detect(lines: normalizedLines)
        #expect(fromPixels.detected)
        #expect(fromNormalized.detected)
        #expect(abs(fromPixels.y - fromNormalized.y) < 1e-6)
        #expect(abs(fromPixels.height - fromNormalized.height) < 1e-6)
    }

    // MARK: - D01 录制 fixture 黄金（与 C++ 黄金用例同口径）

    @Test
    func fixtureWatermarkBandGolden() throws {
        let lines = try loadBandFixture("watermark_band_32s")
        let result = SubtitleBandDetector.detect(lines: lines)
        #expect(result.detected)
        #expect(result.y > 0.78)
        #expect(result.y < 0.84)
        #expect(result.height > 0.09)
        #expect(result.height < 0.16)
        #expect(result.previewText.contains("字幕"))
        #expect(result.diagnostics.excludedWatermarkLines >= 1)
    }

    @Test
    func fixtureScrollingWatermarkGolden() throws {
        let lines = try loadBandFixture("scrolling_watermark_32s")
        let result = SubtitleBandDetector.detect(lines: lines)
        #expect(result.detected)
        #expect(result.y > 0.77)
        #expect(result.y < 0.84)
        #expect(result.height > 0.09)
        #expect(result.height < 0.17)
        #expect(!result.previewText.contains("98Tang"))
    }

    @Test
    func fixtureBand1080pGolden() throws {
        let lines = try loadBandFixture("band_1080p")
        let result = SubtitleBandDetector.detect(lines: lines)
        #expect(result.detected)
        #expect(result.y > 0.84)
        #expect(result.y < 0.92)
        #expect(result.height > 0.08)
        #expect(result.height < 0.18)
    }

    @Test
    func fixtureZootopiaGolden() throws {
        let lines = try loadBandFixture("Zootopia_clip_1080p")
        let result = SubtitleBandDetector.detect(lines: lines)
        #expect(result.detected)
        #expect(result.y > 0.72)
        #expect(result.y < 0.79)
        #expect(result.height > 0.10)
        #expect(result.height < 0.18)
    }

    // MARK: - 辅助

    private func makeLine(
        _ frame: Int,
        _ x: Double,
        _ y: Double,
        _ width: Double,
        _ height: Double,
        _ text: String,
        confidence: Double = 0.95
    ) -> SubtitleBandLine {
        SubtitleBandLine(
            frameIndex: frame,
            x: x,
            y: y,
            width: width,
            height: height,
            text: text,
            confidence: confidence
        )
    }
}

// MARK: - fixture 读取

private func bandFixtureDirectory() -> URL {
    URL(fileURLWithPath: #filePath)
        .deletingLastPathComponent()
        .deletingLastPathComponent()
        .deletingLastPathComponent()
        .deletingLastPathComponent()
        .deletingLastPathComponent()
        .appendingPathComponent("cpp/tests/fixtures/subtitle_band", isDirectory: true)
}

private func loadBandFixture(_ name: String) throws -> [SubtitleBandLine] {
    let url = bandFixtureDirectory().appendingPathComponent("\(name).json")
    let data = try Data(contentsOf: url)
    guard let root = try JSONSerialization.jsonObject(with: data) as? [String: Any],
          let frames = root["frames"] as? [[String: Any]] else {
        throw CocoaError(.fileReadCorruptFile)
    }
    var lines: [SubtitleBandLine] = []
    for frame in frames {
        let frameIndex = intValue(frame["frame_index"])
        guard let items = frame["lines"] as? [[String: Any]] else { continue }
        for item in items {
            lines.append(
                SubtitleBandLine(
                    frameIndex: frameIndex,
                    x: doubleValue(item["x"]),
                    y: doubleValue(item["y"]),
                    width: doubleValue(item["width"]),
                    height: doubleValue(item["height"]),
                    text: (item["text"] as? String) ?? "",
                    confidence: doubleValue(item["confidence"])
                )
            )
        }
    }
    return lines
}

private func doubleValue(_ value: Any?) -> Double {
    (value as? NSNumber)?.doubleValue ?? 0
}

private func intValue(_ value: Any?) -> Int {
    (value as? NSNumber)?.intValue ?? 0
}
