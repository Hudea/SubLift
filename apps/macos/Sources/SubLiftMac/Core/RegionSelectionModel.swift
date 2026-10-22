import CoreGraphics
import Foundation

/// feat-022：Vision 检出的单个文字候选框。
struct TextBoxCandidate: Identifiable, Equatable {
    let id: Int
    let pixelRect: CGRect
    let textPreview: String
    let confidence: Float
    let colorIndex: Int
}

/// feat-022：字幕区域多选状态（预览叠加 + 全宽 Y 带推算）。
///
/// Phase 21 D04：detect 改为聚合全部采样帧的 Vision 候选，交给
/// SubtitleBandDetector 做多帧自适应条带决策；代表帧候选仍用于展示与手动微调。
@MainActor
final class RegionSelectionModel: ObservableObject {

    enum Status: Equatable {
        case idle
        case detecting
        case ready
        case failed(String)
    }

    @Published private(set) var status: Status = .idle
    @Published private(set) var candidates: [TextBoxCandidate] = []
    @Published var selectedIds: Set<Int> = []
    @Published private(set) var mergedRegion: CGRect?
    /// 最终采用的代表帧时间（秒）；供 UI 展示。
    @Published private(set) var sampleSecondsUsed: Double?
    /// 实际尝试的抽帧次数。
    @Published private(set) var sampleAttemptCount: Int = 0

    // MARK: Phase 21 D04 可观察诊断

    /// 自适应条带检测诊断（锚点帧/行高/支持帧/排除原因）。
    @Published private(set) var bandDiagnostics: SubtitleBandDiagnostics?
    /// 被判定为水印/台标并被排除的候选行数量。
    @Published private(set) var excludedWatermarkCount: Int = 0
    /// 自动选中集合（不含 band 外/水印行）；供测试与 UI 对照用户手动改动。
    @Published private(set) var autoSelectedIds: Set<Int> = []

    private var videoWidth: Int = 0
    private var videoHeight: Int = 0
    /// 自适应 band 对应的视频像素区域；仅在自动选中集合未被用户改动时作为合并区域。
    private var bandDerivedRegion: CGRect?

    /// 供 UI 展示的当前视频像素宽度（合并区域摘要区分全宽/收窄）；0 表示未知。
    var videoWidthForPresentation: Int { videoWidth }

    func clear() {
        status = .idle
        candidates = []
        selectedIds = []
        mergedRegion = nil
        sampleSecondsUsed = nil
        sampleAttemptCount = 0
        videoWidth = 0
        videoHeight = 0
        bandDiagnostics = nil
        excludedWatermarkCount = 0
        autoSelectedIds = []
        bandDerivedRegion = nil
    }

    /// 在多时间点抽帧，聚合全部采样帧的 Vision 候选后做自适应条带检测。
    ///
    /// 与 C++ D02 同口径：无字幕锚点时 detect 仍展示候选，但不自动选中/不返回水印框
    /// （fail-closed，由用户手动点选后可继续）。
    func detect(
        url: URL,
        videoWidth: Int,
        videoHeight: Int,
        durationMs: Int
    ) async {
        clear()
        guard videoWidth > 0, videoHeight > 0 else {
            status = .failed("视频分辨率无效")
            return
        }

        self.videoWidth = videoWidth
        self.videoHeight = videoHeight
        status = .detecting

        let durationSeconds = max(0, Double(durationMs) / 1000.0)
        let timestamps = RegionFramePicker.sampleTimestamps(durationSeconds: durationSeconds)

        var frameSamples: [SampledFrameLines] = []
        var bestScore = -1.0
        var bestFrame: SampledFrameLines?
        var bestSeconds: Double?
        var attempts = 0
        var lastError: VisionTextDetector.DetectError?

        for (frameIndex, seconds) in timestamps.enumerated() {
            guard let cgImage = await FrameSampler.captureFrameAt(url: url, seconds: seconds) else {
                continue
            }
            attempts += 1
            sampleAttemptCount = attempts

            let detections: [VisionTextDetector.Detection]
            do {
                detections = try await Task.detached(priority: .userInitiated) {
                    try VisionTextDetector.detect(in: cgImage)
                }.value
            } catch let error as VisionTextDetector.DetectError {
                lastError = error
                // 有画面但无文字：作为「该采样帧无字幕」的有效证据参与间歇性判断。
                if case .noResults = error {
                    frameSamples.append(SampledFrameLines(frameIndex: frameIndex, lines: []))
                }
                continue
            } catch {
                lastError = .performFailed(error.localizedDescription)
                continue
            }

            let pixelRects = detections.map { detection in
                VideoCoordinateMapper.visionNormalizedRectToVideoPixels(
                    detection.normalizedRect,
                    videoWidth: videoWidth,
                    videoHeight: videoHeight
                )
            }
            let lines = detections.indices.map { index in
                SubtitleBandLine(
                    frameIndex: frameIndex,
                    pixelRect: pixelRects[index],
                    text: detections[index].text,
                    confidence: detections[index].confidence,
                    videoWidth: videoWidth,
                    videoHeight: videoHeight
                )
            }
            let sample = SampledFrameLines(frameIndex: frameIndex, lines: lines)
            frameSamples.append(sample)

            let scored = pixelRects.indices.map { index in
                (pixelRect: pixelRects[index], confidence: detections[index].confidence)
            }
            let score = RegionFramePicker.subtitlePresenceScore(
                candidates: scored,
                videoHeight: videoHeight
            )
            if score > bestScore {
                bestScore = score
                bestFrame = sample
                bestSeconds = seconds
            }

            // Phase 21 D04：不再按单帧得分提前停止——条带决策需要多帧证据。
        }

        guard attempts > 0 else {
            status = .failed("无法抽取代表帧")
            return
        }
        guard let representative = bestFrame else {
            if case .performFailed(let message)? = lastError {
                status = .failed("Vision 检测失败: \(message)")
            } else {
                status = .failed("多帧均未检测到文字候选框，可拖到有字幕处后点「重检当前帧」")
            }
            return
        }

        // 与 C++ region_detector 同口径：传实际成功采样帧数（含无文字帧，不含抽帧/检测失败帧），
        // 避免尾部空帧导致间歇性分母少算。
        var bandConfig = SubtitleBandConfig.standard
        bandConfig.sampleFramesHint = frameSamples.count
        let aggregation = RegionSelectionAggregator.aggregate(
            frames: frameSamples,
            representativeFrameIndex: representative.frameIndex,
            videoWidth: videoWidth,
            videoHeight: videoHeight,
            config: bandConfig
        )
        sampleSecondsUsed = bestSeconds
        applyAggregation(aggregation, representativeFrame: representative)
        status = .ready
    }

    /// 对指定时间点单帧检测（用于「当前播放头重检」）。
    ///
    /// K=1 仍走同一自适应条带算法（单帧锚点允许 support=1，但会按常驻/孤证降权）。
    func detectAt(
        url: URL,
        videoWidth: Int,
        videoHeight: Int,
        sampleSeconds: Double
    ) async {
        // 保留尺寸，重置候选状态
        candidates = []
        selectedIds = []
        mergedRegion = nil
        sampleSecondsUsed = nil
        sampleAttemptCount = 0
        bandDiagnostics = nil
        excludedWatermarkCount = 0
        autoSelectedIds = []
        bandDerivedRegion = nil

        guard videoWidth > 0, videoHeight > 0 else {
            status = .failed("视频分辨率无效")
            return
        }

        self.videoWidth = videoWidth
        self.videoHeight = videoHeight
        status = .detecting

        let seconds = max(0, sampleSeconds)
        guard let cgImage = await FrameSampler.captureFrameAt(url: url, seconds: seconds) else {
            status = .failed("无法抽取代表帧")
            return
        }
        sampleAttemptCount = 1

        let detections: [VisionTextDetector.Detection]
        do {
            detections = try await Task.detached(priority: .userInitiated) {
                try VisionTextDetector.detect(in: cgImage)
            }.value
        } catch let error as VisionTextDetector.DetectError {
            switch error {
            case .noResults:
                status = .failed("当前帧未检测到文字，请换到有字幕的画面再试")
            case .performFailed(let message):
                status = .failed("Vision 检测失败: \(message)")
            }
            return
        } catch {
            status = .failed("Vision 检测失败: \(error.localizedDescription)")
            return
        }

        let pixelRects = detections.map { detection in
            VideoCoordinateMapper.visionNormalizedRectToVideoPixels(
                detection.normalizedRect,
                videoWidth: videoWidth,
                videoHeight: videoHeight
            )
        }
        let lines = detections.indices.map { index in
            SubtitleBandLine(
                frameIndex: 0,
                pixelRect: pixelRects[index],
                text: detections[index].text,
                confidence: detections[index].confidence,
                videoWidth: videoWidth,
                videoHeight: videoHeight
            )
        }
        let sample = SampledFrameLines(frameIndex: 0, lines: lines)

        sampleSecondsUsed = seconds
        var bandConfig = SubtitleBandConfig.standard
        bandConfig.sampleFramesHint = 1
        let aggregation = RegionSelectionAggregator.aggregate(
            frames: [sample],
            representativeFrameIndex: 0,
            videoWidth: videoWidth,
            videoHeight: videoHeight,
            config: bandConfig
        )
        applyAggregation(aggregation, representativeFrame: sample)
        status = .ready
    }

    func toggleSelection(id: Int) {
        guard candidates.contains(where: { $0.id == id }) else { return }
        if selectedIds.contains(id) {
            selectedIds.remove(id)
        } else {
            selectedIds.insert(id)
        }
        refreshMergedRegion()
    }

    func setSelection(id: Int, selected: Bool) {
        guard candidates.contains(where: { $0.id == id }) else { return }
        if selected {
            selectedIds.insert(id)
        } else {
            selectedIds.remove(id)
        }
        refreshMergedRegion()
    }

    // MARK: - Private

    /// 应用多帧聚合结果：候选展示保持「代表帧」语义，自动选中/合并区域来自 band。
    private func applyAggregation(
        _ aggregation: RegionSelectionAggregation,
        representativeFrame: SampledFrameLines
    ) {
        candidates = representativeFrame.lines.enumerated().map { index, line in
            let pixelRect = line.pixelRect(videoWidth: videoWidth, videoHeight: videoHeight)
            return TextBoxCandidate(
                id: index + 1,
                pixelRect: pixelRect,
                textPreview: line.text.isEmpty ? "（空）" : line.text,
                confidence: Float(line.confidence),
                colorIndex: index
            )
        }

        autoSelectedIds = aggregation.selectedCandidateIds
        selectedIds = autoSelectedIds
        bandDiagnostics = aggregation.diagnostics
        excludedWatermarkCount = aggregation.excludedWatermarkCount
        bandDerivedRegion = aggregation.mergedRegion
        refreshMergedRegion()
    }

    private func refreshMergedRegion() {
        // 自动选中集合未被用户改动时，合并区域以自适应 band 为准（可能 X 收窄）。
        if let bandDerivedRegion, selectedIds == autoSelectedIds {
            mergedRegion = bandDerivedRegion
            return
        }
        mergedRegion = RegionMerger.mergedFullWidthRegion(
            candidates: candidates.map { ($0.id, $0.pixelRect) },
            selectedIds: selectedIds,
            videoWidth: videoWidth,
            videoHeight: videoHeight
        )
    }

    /// feat-022b：供 start_job 使用的 region_box；无合并区域时返回 nil。
    ///
    /// Phase 21 D04：band 自动选区会保留 X/W（侧边水印 X 收窄时裁剪起点不再强制全宽）；
    /// 用户手动改动选择时保持原 RegionMerger 全宽语义。
    func regionBoxForIPC() -> RegionBox? {
        if let bandDerivedRegion, selectedIds == autoSelectedIds {
            return RegionSelectionIPCBox.make(
                bandDerivedRegion,
                videoWidth: videoWidth,
                videoHeight: videoHeight
            )
        }
        return RegionMerger.regionBoxFromMergedRegion(
            mergedRegion,
            videoWidth: videoWidth,
            videoHeight: videoHeight
        )
    }

    /// feat-034b：供 start_job 使用的 subtitle_profile（相对 region crop）。
    func subtitleProfileForIPC() -> SubtitleProfilePayload? {
        guard let regionBox = regionBoxForIPC() else { return nil }
        return RegionMerger.subtitleProfileFromSelection(
            candidates: candidates.map { ($0.id, $0.pixelRect, $0.textPreview) },
            selectedIds: selectedIds,
            regionBox: regionBox
        )
    }
}

/// Phase 21 D04：保留 X/W 的 region_box 换算。
///
/// 既有 RegionMerger.regionBoxFromMergedRegion 固定 x=0、width=videoWidth（全宽裁剪契约）；
/// 自适应 band 的 X 收窄需要把裁剪起点真正传给 server（协议允许非零 x），故单独提供。
enum RegionSelectionIPCBox {

    static func make(_ rect: CGRect?, videoWidth: Int, videoHeight: Int) -> RegionBox? {
        guard let rect, videoWidth > 0, videoHeight > 0 else { return nil }
        let x = max(0, min(Int(rect.origin.x.rounded(.down)), videoWidth - 1))
        let y = max(0, min(Int(rect.origin.y.rounded(.down)), videoHeight - 1))
        let maxX = max(x + 1, min(Int(rect.maxX.rounded(.up)), videoWidth))
        let maxY = max(y + 1, min(Int(rect.maxY.rounded(.up)), videoHeight))
        return [x, y, maxX - x, maxY - y]
    }
}
