import CoreGraphics
import Foundation

// MARK: - 输入：多帧 OCR 行证据

/// Phase 21 D02/D04：单个采样帧中的一条 OCR 文本行证据（归一化坐标，原点左上）。
///
/// 与 C++ sublift::SubtitleBandLine 同口径：x/y/width/height ∈ [0,1]。
/// macOS 侧可直接用 Vision 归一化框（左上原点）或视频像素框构建。
struct SubtitleBandLine: Equatable {
    let frameIndex: Int
    let x: Double
    let y: Double
    let width: Double
    let height: Double
    let text: String
    let confidence: Double

    init(
        frameIndex: Int,
        x: Double,
        y: Double,
        width: Double,
        height: Double,
        text: String,
        confidence: Double
    ) {
        self.frameIndex = frameIndex
        self.x = x
        self.y = y
        self.width = width
        self.height = height
        self.text = text
        self.confidence = confidence
    }

    /// 归一化 rect 构造（左上原点）。
    init(frameIndex: Int, normalizedRect: CGRect, text: String, confidence: Float) {
        self.init(
            frameIndex: frameIndex,
            x: Double(normalizedRect.origin.x),
            y: Double(normalizedRect.origin.y),
            width: Double(normalizedRect.size.width),
            height: Double(normalizedRect.size.height),
            text: text,
            confidence: Double(confidence)
        )
    }

    /// 视频像素 rect 构造（左上原点）；内部除以画面宽高得到归一化坐标。
    init(
        frameIndex: Int,
        pixelRect: CGRect,
        text: String,
        confidence: Float,
        videoWidth: Int,
        videoHeight: Int
    ) {
        let width = Double(max(videoWidth, 1))
        let height = Double(max(videoHeight, 1))
        self.init(
            frameIndex: frameIndex,
            x: Double(pixelRect.origin.x) / width,
            y: Double(pixelRect.origin.y) / height,
            width: Double(pixelRect.size.width) / width,
            height: Double(pixelRect.size.height) / height,
            text: text,
            confidence: Double(confidence)
        )
    }

    var centerX: Double { x + width / 2.0 }
    var centerY: Double { y + height / 2.0 }

    var normalizedRect: CGRect {
        CGRect(x: CGFloat(x), y: CGFloat(y), width: CGFloat(width), height: CGFloat(height))
    }

    /// 归一化 → 视频像素（左上原点）。
    func pixelRect(videoWidth: Int, videoHeight: Int) -> CGRect {
        let width = CGFloat(max(videoWidth, 1))
        let height = CGFloat(max(videoHeight, 1))
        return CGRect(
            x: CGFloat(x) * width,
            y: CGFloat(y) * height,
            width: CGFloat(self.width) * width,
            height: CGFloat(self.height) * height
        )
    }
}

/// 一个采样帧的 OCR 行；空数组表示「抽到了画面，但没有文字」（仍是有效采样证据）。
///
/// frameIndex 为采样序号（0 起），跨帧支持度按它去重。
struct SampledFrameLines: Equatable {
    let frameIndex: Int
    let lines: [SubtitleBandLine]

    init(frameIndex: Int, lines: [SubtitleBandLine]) {
        self.frameIndex = frameIndex
        self.lines = lines
    }
}

// MARK: - 参数（与 C++ SubtitleBandConfig 默认值同口径）

/// 自适应字幕条带参数。默认值来自 Phase 21 D01 的 4 片源原型标定，
/// 与 C++ sublift::SubtitleBandConfig 保持一致（跨端同口径）。
struct SubtitleBandConfig: Equatable {
    /// 采样帧数提示；0 表示由输入行的 max(frameIndex)+1 推导。
    /// 区域检测器应传入实际采样帧数（尾部空帧不产生行，推导会少算）。
    var sampleFramesHint: Int = 0
    var minConfidence: Double = 0.40
    var minLineHeight: Double = 0.008
    var maxLineHeight: Double = 0.11
    var minCenterY: Double = 0.50
    var maxCenterY: Double = 0.985

    var anchorMinScore: Double = 1.0
    var expandMinScore: Double = 0.9
    var expandMaxCenterOffset: Double = 0.30
    var expandMaxGapFactor: Double = 0.9
    var expandMinGap: Double = 0.012
    var expandHeightRatioMin: Double = 0.5
    var expandHeightRatioMax: Double = 1.8

    var padYFactor: Double = 0.30
    var padYMin: Double = 0.008
    var padXFactor: Double = 0.15
    var padXMin: Double = 0.015
    var minBandHeight: Double = 0.03

    /// 侧边常驻水印存在时，是否允许把 X 从全宽收窄到字幕行范围。
    var enableSideWatermarkTrim: Bool = true
    var sideWatermarkMinSupportRatio: Double = 0.9
    var sideWatermarkEdgeCenter: Double = 0.22
    var sideWatermarkMaxWidth: Double = 0.35
    var sideWatermarkMinGap: Double = 0.02
    var sideTrimMinWidth: Double = 0.60
    var sideTrimOverlapTolerance: Double = 0.01

    static let standard = SubtitleBandConfig()
}

// MARK: - 输出

/// 归一化字幕带（左上原点）。X 默认 0..1；检测到侧边常驻水印族时可能收窄。
struct SubtitleBand: Equatable {
    let x: Double
    let y: Double
    let width: Double
    let height: Double

    var rect: CGRect {
        CGRect(x: CGFloat(x), y: CGFloat(y), width: CGFloat(width), height: CGFloat(height))
    }

    var isFullWidth: Bool { width >= 0.999 }
}

/// 条带检测的可解释诊断（用于测试、日志与 UI 提示）。
struct SubtitleBandDiagnostics: Equatable {
    var totalLines: Int = 0
    var sampleFrames: Int = 0
    var anchorFrameIndex: Int = -1
    var anchorCenterY: Double = 0.0
    var anchorLineHeight: Double = 0.0
    var anchorSupportFrames: Int = 0
    var includedLines: Int = 0
    var excludedWatermarkLines: Int = 0
    var excludedTexts: [String] = []
    var sideWatermarkTrimmed: Bool = false
    var reason: String = ""

    /// 便捷：被排除的水印行数（D04 需求命名）。
    var excludedWatermarkCount: Int { excludedWatermarkLines }
}

/// 自适应条带检测结果（字段与 C++ sublift::SubtitleBandResult 对齐）。
struct SubtitleBandResult: Equatable {
    var detected: Bool = false
    var x: Double = 0.0
    var y: Double = 0.70
    var width: Double = 1.0
    var height: Double = 0.25
    var previewText: String = ""
    var confidence: Double = 0.0
    var totalCandidates: Int = 0
    var diagnostics: SubtitleBandDiagnostics = SubtitleBandDiagnostics()

    /// 归一化 band；未检出时为 nil（fail-closed，不返回水印框）。
    var band: SubtitleBand? {
        guard detected else { return nil }
        return SubtitleBand(x: x, y: y, width: width, height: height)
    }
}

/// 检测结果 + 被吸收行在输入数组中的下标（供选区聚合判断代表帧候选是否属于字幕带）。
struct SubtitleBandOutcome: Equatable {
    let result: SubtitleBandResult
    let includedLineIndices: Set<Int>
}

// MARK: - 检测器

/// 多帧居中式自适应字幕条带检测（纯逻辑：无 IO / Vision / AVFoundation 依赖）。
///
/// 算法与阈值见 docs/design/subtitle-band-detection.md，与 C++ D02 的
/// sublift::detect_subtitle_band 同口径：
/// 1) 行级外观分 + 跨帧几何支持度；
/// 2) 以「外观分 × 间歇性」选字幕锚点行；
/// 3) 从锚点中心向外逐行扩散，自适应 padding 得到 band 高度；
/// 4) 默认 X 全宽，仅在检测到侧边常驻水印族时收窄，宁宽勿窄。
///
/// 无锚点（例如全片只有 URL/台标行）时返回 detected=false，reason 可解释；
/// 不会把水印框当字幕区返回。
enum SubtitleBandDetector {

    // MARK: 公开 API

    /// 多帧居中式自适应字幕条带检测。
    static func detect(
        lines: [SubtitleBandLine],
        config: SubtitleBandConfig = .standard
    ) -> SubtitleBandResult {
        classify(lines: lines, config: config).result
    }

    /// 同 detect，额外返回被吸收行下标（内部用于选区聚合）。
    static func classify(
        lines: [SubtitleBandLine],
        config: SubtitleBandConfig = .standard
    ) -> SubtitleBandOutcome {
        var result = SubtitleBandResult()
        result.diagnostics.totalLines = lines.count
        let sampleFrames = sampleFrameCount(lines, config: config)
        result.diagnostics.sampleFrames = sampleFrames

        guard !lines.isEmpty else {
            result.diagnostics.reason = "empty_input"
            return SubtitleBandOutcome(result: result, includedLineIndices: [])
        }

        // ---- 1) 锚点行：外观分 × 间歇性 ----
        var anchorIndex: Int?
        var bestAnchorScore = -1.0
        var anchorSupport = 0
        for index in lines.indices {
            let line = lines[index]
            guard isValidLine(line, config: config) else { continue }
            guard !containsWatermarkDomain(line.text) else { continue }
            let score = lineScore(line)
            guard score >= config.anchorMinScore else { continue }
            let support = supportFrames(lines, index: index).frames
            let total = score * intermittencyFactor(support: support, sampleFrames: sampleFrames)
            if anchorIndex == nil || total > bestAnchorScore + 1e-12 {
                anchorIndex = index
                bestAnchorScore = total
                anchorSupport = support
            }
        }

        guard let anchor = anchorIndex else {
            result.diagnostics.reason = "no_subtitle_anchor"
            return SubtitleBandOutcome(result: result, includedLineIndices: [])
        }

        let anchorHeight = lines[anchor].height
        let anchorCenterX = lines[anchor].centerX
        result.diagnostics.anchorFrameIndex = lines[anchor].frameIndex
        result.diagnostics.anchorCenterY = lines[anchor].centerY
        result.diagnostics.anchorLineHeight = anchorHeight
        result.diagnostics.anchorSupportFrames = anchorSupport

        // ---- 2) 从锚点中心向外逐行扩散 ----
        var included = [Bool](repeating: false, count: lines.count)
        included[anchor] = true
        var changed = true
        while changed {
            changed = false
            var bandLow = 1.0
            var bandHigh = 0.0
            for index in lines.indices where included[index] {
                bandLow = min(bandLow, lines[index].y)
                bandHigh = max(bandHigh, lines[index].y + lines[index].height)
            }
            let maxGap = max(config.expandMinGap, config.expandMaxGapFactor * anchorHeight)
            for index in lines.indices where !included[index] {
                let line = lines[index]
                guard isValidLine(line, config: config) else { continue }
                guard !containsWatermarkDomain(line.text) else { continue }
                guard lineScore(line) >= config.expandMinScore else { continue }
                let ratio = line.height / anchorHeight
                guard ratio >= config.expandHeightRatioMin, ratio <= config.expandHeightRatioMax else { continue }
                guard abs(line.centerX - anchorCenterX) <= config.expandMaxCenterOffset else { continue }

                let lineLow = line.y
                let lineHigh = line.y + line.height
                let gap: Double
                if lineLow >= bandHigh {
                    gap = lineLow - bandHigh
                } else if bandLow >= lineHigh {
                    gap = bandLow - lineHigh
                } else {
                    gap = 0.0
                }
                guard gap <= maxGap else { continue }
                included[index] = true
                changed = true
            }
        }

        // ---- 3) band 几何 + 自适应 padding ----
        var low = 1.0
        var high = 0.0
        var subtitleMinX = 1.0
        var subtitleMaxX = 0.0
        var confidenceSum = 0.0
        var includedCount = 0
        var previewItems: [(y: Double, text: String)] = []
        for index in lines.indices where included[index] {
            let line = lines[index]
            low = min(low, line.y)
            high = max(high, line.y + line.height)
            subtitleMinX = min(subtitleMinX, line.x)
            subtitleMaxX = max(subtitleMaxX, line.x + line.width)
            confidenceSum += clamp01(line.confidence)
            includedCount += 1
            if !containsWatermarkDomain(line.text) {
                previewItems.append((line.y, line.text))
            }
        }

        let padY = max(config.padYMin, config.padYFactor * anchorHeight)
        var y0 = clamp01(low - padY)
        var y1 = clamp01(high + padY)
        if y1 - y0 < config.minBandHeight {
            let center = (y0 + y1) / 2.0
            y0 = clamp01(center - config.minBandHeight / 2.0)
            y1 = clamp01(center + config.minBandHeight / 2.0)
            if y1 - y0 < config.minBandHeight {
                if y0 <= 0.0 {
                    y1 = min(1.0, config.minBandHeight)
                } else {
                    y0 = max(0.0, 1.0 - config.minBandHeight)
                }
            }
        }

        // ---- 4) X 策略：默认全宽；仅侧边常驻水印存在时收窄 ----
        var bandX = 0.0
        var bandWidth = 1.0
        var sideTrimmed = false
        if config.enableSideWatermarkTrim, includedCount > 0 {
            let subtitleWidth = subtitleMaxX - subtitleMinX
            var sideFound = false
            var sideMinX = 1.0
            var sideMaxX = 0.0
            for index in lines.indices where !included[index] {
                let line = lines[index]
                guard isValidLine(line, config: config) else { continue }
                let sideLike = containsWatermarkDomain(line.text) || isEdgeSmall(line, config: config)
                guard sideLike else { continue }
                let support = supportFrames(lines, index: index)
                let ratio = sampleFrames > 0 ? Double(support.frames) / Double(sampleFrames) : 1.0
                guard ratio >= config.sideWatermarkMinSupportRatio else { continue }
                var x0 = line.x
                var x1 = line.x + line.width
                for match in support.matches {
                    x0 = min(x0, lines[match].x)
                    x1 = max(x1, lines[match].x + lines[match].width)
                }
                if x1 <= subtitleMinX - config.sideWatermarkMinGap
                    || x0 >= subtitleMaxX + config.sideWatermarkMinGap {
                    sideFound = true
                    sideMinX = min(sideMinX, x0)
                    sideMaxX = max(sideMaxX, x1)
                }
            }

            if sideFound {
                let padX = max(config.padXMin, config.padXFactor * subtitleWidth)
                var trimMinX = clamp01(subtitleMinX - padX)
                var trimMaxX = clamp01(subtitleMaxX + padX)
                if trimMaxX - trimMinX < config.sideTrimMinWidth {
                    let center = (trimMinX + trimMaxX) / 2.0
                    trimMinX = clamp01(center - config.sideTrimMinWidth / 2.0)
                    trimMaxX = clamp01(center + config.sideTrimMinWidth / 2.0)
                    if trimMaxX - trimMinX < config.sideTrimMinWidth {
                        if trimMinX <= 0.0 {
                            trimMaxX = min(1.0, config.sideTrimMinWidth)
                        } else {
                            trimMinX = max(0.0, 1.0 - config.sideTrimMinWidth)
                        }
                    }
                }
                let overlap = min(trimMaxX, sideMaxX) - max(trimMinX, sideMinX)
                if overlap <= config.sideTrimOverlapTolerance {
                    bandX = trimMinX
                    bandWidth = trimMaxX - trimMinX
                    sideTrimmed = true
                }
            }
        }

        // ---- 5) 诊断：被排除的水印/台标行 ----
        var excluded = 0
        var excludedTexts: [String] = []
        for index in lines.indices where !included[index] {
            let line = lines[index]
            guard isValidLine(line, config: config) else { continue }
            var watermarkLike = containsWatermarkDomain(line.text)
            if !watermarkLike, isEdgeSmall(line, config: config) {
                let support = supportFrames(lines, index: index).frames
                let ratio = sampleFrames > 0 ? Double(support) / Double(sampleFrames) : 1.0
                watermarkLike = ratio >= config.sideWatermarkMinSupportRatio
            }
            guard watermarkLike else { continue }
            excluded += 1
            if excludedTexts.count < 8 {
                excludedTexts.append(line.text)
            }
        }

        result.detected = true
        result.x = bandX
        result.y = y0
        result.width = bandWidth
        result.height = max(0.0, y1 - y0)
        result.confidence = includedCount > 0 ? confidenceSum / Double(includedCount) : 0.0
        result.totalCandidates = includedCount
        var previewText = ""
        sortAndDedupePreview(&previewItems, into: &previewText)
        result.previewText = previewText
        result.diagnostics.includedLines = includedCount
        result.diagnostics.excludedWatermarkLines = excluded
        result.diagnostics.excludedTexts = excludedTexts
        result.diagnostics.sideWatermarkTrimmed = sideTrimmed

        let includedIndices = Set(lines.indices.filter { included[$0] })
        return SubtitleBandOutcome(result: result, includedLineIndices: includedIndices)
    }

    // MARK: 文本水印标记

    private static let knownTLDs: Set<String> = [
        "com", "net", "org", "cn", "cc", "tv", "me", "top", "vip", "xyz",
        "info", "io", "co", "la", "in", "us", "uk", "jp", "kr",
    ]

    /// 判断文本是否包含水印常见 URL/域名标记（http(s)://、www.、token.tld）。
    ///
    /// 规则刻意保守：只匹配常见 TLD 且 token 至少 2 个 ASCII 字符，避免误判正常对白里的
    /// 缩写/句点。与 C++ contains_watermark_domain 同规则（D03 行级水印规则复用）。
    static func containsWatermarkDomain(_ text: String) -> Bool {
        let lowered = text.lowercased()
        if lowered.contains("http://") || lowered.contains("https://") || lowered.contains("www.") {
            return true
        }
        let bytes = Array(lowered.utf8)
        let dotByte = UInt8(ascii: ".")
        let hyphenByte = UInt8(ascii: "-")
        for dot in bytes.indices where bytes[dot] == dotByte {
            var begin = dot
            while begin > 0 {
                let byte = bytes[begin - 1]
                if isASCIIAlnum(byte) || byte == hyphenByte {
                    begin -= 1
                } else {
                    break
                }
            }
            if dot - begin < 2 { continue }
            var end = dot + 1
            while end < bytes.count, isASCIIAlpha(bytes[end]) {
                end += 1
            }
            let tldLength = end - (dot + 1)
            if tldLength < 2 || tldLength > 6 { continue }
            let tld = String(decoding: bytes[(dot + 1)..<end], as: UTF8.self)
            if knownTLDs.contains(tld) {
                return true
            }
        }
        return false
    }

    // MARK: 行级打分与几何匹配

    /// 行级「像字幕」外观分（与设计文档 §2.1 一致）。不做 [0,1] 截断，URL/贴边扣分可为负。
    static func lineScore(_ line: SubtitleBandLine) -> Double {
        let cx = line.centerX
        let cy = line.centerY
        var score = 0.0
        if cx >= 0.33, cx <= 0.67 {
            score += 1.0
        } else if cx >= 0.25, cx <= 0.75 {
            score += 0.55
        } else {
            score += 0.05
        }
        if line.width >= 0.15 {
            score += 1.0
        } else if line.width >= 0.08 {
            score += 0.55
        } else {
            score += 0.1
        }
        if cy >= 0.55, cy <= 0.96 {
            score += 1.0
        } else if cy >= 0.45 {
            score += 0.4
        }
        score += 0.3 * clamp01(line.confidence)
        if containsWatermarkDomain(line.text) {
            score -= 1.6
        }
        if (cx < 0.15 || cx > 0.85), line.width < 0.20 {
            score -= 0.8
        }
        return score
    }

    // MARK: 私有

    private static func clamp01(_ value: Double) -> Double {
        max(0.0, min(1.0, value))
    }

    private static func isASCIIAlnum(_ byte: UInt8) -> Bool {
        (byte >= 0x30 && byte <= 0x39) || isASCIIAlpha(byte)
    }

    private static func isASCIIAlpha(_ byte: UInt8) -> Bool {
        (byte >= 0x41 && byte <= 0x5A) || (byte >= 0x61 && byte <= 0x7A)
    }

    private static func isValidLine(_ line: SubtitleBandLine, config: SubtitleBandConfig) -> Bool {
        guard line.width > 0.0, line.height > 0.0 else { return false }
        guard line.confidence >= config.minConfidence else { return false }
        guard line.height >= config.minLineHeight, line.height <= config.maxLineHeight else {
            return false
        }
        let cy = line.centerY
        return cy >= config.minCenterY && cy <= config.maxCenterY
    }

    /// 贴边小框（角落台标/水印的几何特征）。
    private static func isEdgeSmall(_ line: SubtitleBandLine, config: SubtitleBandConfig) -> Bool {
        let cx = line.centerX
        let onEdge = cx < config.sideWatermarkEdgeCenter
            || cx > 1.0 - config.sideWatermarkEdgeCenter
        return onEdge && line.width < config.sideWatermarkMaxWidth
    }

    /// 跨帧几何匹配：同一位置/字号的行视为同一「轨道」。
    private static func geometryMatches(_ reference: SubtitleBandLine, _ other: SubtitleBandLine) -> Bool {
        if abs(reference.centerY - other.centerY) > 0.6 * reference.height {
            return false
        }
        if !(other.height >= 0.55 * reference.height && other.height <= 1.7 * reference.height) {
            return false
        }
        return abs(reference.centerX - other.centerX) <= 0.22
    }

    /// 一条行的跨帧支持度（不同 frameIndex 数，含自身）；同时返回几何匹配命中行下标。
    private static func supportFrames(
        _ lines: [SubtitleBandLine],
        index: Int
    ) -> (frames: Int, matches: [Int]) {
        var frameIndices = Set<Int>()
        var matches: [Int] = []
        let reference = lines[index]
        for other in lines.indices where geometryMatches(reference, lines[other]) {
            frameIndices.insert(lines[other].frameIndex)
            matches.append(other)
        }
        return (frameIndices.count, matches)
    }

    /// 间歇性因子：字幕是间歇出现的；每帧常驻的文本（水印/台标）降权，单帧孤证也降权。
    private static func intermittencyFactor(support: Int, sampleFrames: Int) -> Double {
        guard sampleFrames > 0 else { return 1.0 }
        let ratio = Double(support) / Double(sampleFrames)
        if ratio > 0.90 { return 0.45 }
        if ratio < 0.25 { return 0.60 }
        return 1.0
    }

    private static func sampleFrameCount(_ lines: [SubtitleBandLine], config: SubtitleBandConfig) -> Int {
        if config.sampleFramesHint > 0 { return config.sampleFramesHint }
        return lines.map { $0.frameIndex + 1 }.max() ?? 0
    }

    private static func sortAndDedupePreview(
        _ items: inout [(y: Double, text: String)],
        into out: inout String
    ) {
        items.sort { $0.y < $1.y }
        var seen: [String] = []
        var emitted = 0
        for item in items {
            guard !item.text.isEmpty else { continue }
            guard !seen.contains(item.text) else { continue }
            seen.append(item.text)
            if !out.isEmpty { out += " " }
            out += item.text
            emitted += 1
            if emitted >= 8 { break }
        }
    }
}

// MARK: - 多帧聚合（RegionSelectionModel 的纯逻辑层）

/// 选区聚合结果：band 决策 + 代表帧自动选中集合 + 像素合并区域。
struct RegionSelectionAggregation: Equatable {
    let bandResult: SubtitleBandResult
    /// 视频像素合并区域；未检出锚点时为 nil（fail-closed，不回退水印框）。
    let mergedRegion: CGRect?
    /// 代表帧候选框自动选中集合（候选 id = 该帧行序号 + 1）。
    let selectedCandidateIds: Set<Int>

    var detected: Bool { bandResult.detected }
    var band: SubtitleBand? { bandResult.band }
    var diagnostics: SubtitleBandDiagnostics { bandResult.diagnostics }
    var excludedWatermarkCount: Int { bandResult.diagnostics.excludedWatermarkLines }
}

/// Phase 21 D04：把「多帧 Vision 候选」聚合成自适应条带与自动选中集合（纯逻辑，可单测）。
enum RegionSelectionAggregator {

    static func aggregate(
        frames: [SampledFrameLines],
        representativeFrameIndex: Int,
        videoWidth: Int,
        videoHeight: Int,
        config: SubtitleBandConfig = .standard
    ) -> RegionSelectionAggregation {
        let allLines = frames.flatMap(\.lines)
        let outcome = SubtitleBandDetector.classify(lines: allLines, config: config)

        guard outcome.result.detected, videoWidth > 0, videoHeight > 0 else {
            return RegionSelectionAggregation(
                bandResult: outcome.result,
                mergedRegion: nil,
                selectedCandidateIds: []
            )
        }

        let mergedRegion = pixelRegion(
            for: outcome.result,
            videoWidth: videoWidth,
            videoHeight: videoHeight
        )

        // 代表帧中「被 band 吸收」的行 → 自动选中；band 外/水印行保持未选。
        var selected: Set<Int> = []
        var offset = 0
        var matchedRepresentative = false
        for frame in frames {
            if !matchedRepresentative, frame.frameIndex == representativeFrameIndex {
                matchedRepresentative = true
                for index in frame.lines.indices where outcome.includedLineIndices.contains(offset + index) {
                    selected.insert(index + 1)
                }
            }
            offset += frame.lines.count
        }

        return RegionSelectionAggregation(
            bandResult: outcome.result,
            mergedRegion: mergedRegion,
            selectedCandidateIds: selected
        )
    }

    /// 归一化 band → 视频像素区域（clamp 到画面内）；无有效区域时 nil。
    static func pixelRegion(
        for result: SubtitleBandResult,
        videoWidth: Int,
        videoHeight: Int
    ) -> CGRect? {
        guard result.detected, videoWidth > 0, videoHeight > 0 else { return nil }
        let width = Double(videoWidth)
        let height = Double(videoHeight)
        let minX = max(0.0, min(result.x * width, width))
        let minY = max(0.0, min(result.y * height, height))
        let maxX = max(minX, min((result.x + result.width) * width, width))
        let maxY = max(minY, min((result.y + result.height) * height, height))
        guard maxX > minX, maxY > minY else { return nil }
        return CGRect(
            x: CGFloat(minX),
            y: CGFloat(minY),
            width: CGFloat(maxX - minX),
            height: CGFloat(maxY - minY)
        )
    }
}
