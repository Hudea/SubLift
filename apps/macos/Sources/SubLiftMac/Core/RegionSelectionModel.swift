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

    private var videoWidth: Int = 0
    private var videoHeight: Int = 0

    func clear() {
        status = .idle
        candidates = []
        selectedIds = []
        mergedRegion = nil
        sampleSecondsUsed = nil
        sampleAttemptCount = 0
        videoWidth = 0
        videoHeight = 0
    }

    /// 在片长上多点抽帧，按「下部字幕存在度」选最佳代表帧再生成候选框。
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

        var bestScore = -1.0
        var bestDetections: [VisionTextDetector.Detection]?
        var bestSeconds: Double?
        var attempts = 0
        var lastError: VisionTextDetector.DetectError?

        for seconds in timestamps {
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
                continue
            } catch {
                lastError = .performFailed(error.localizedDescription)
                continue
            }

            let scored = detections.map { detection -> (pixelRect: CGRect, confidence: Float) in
                let rect = VideoCoordinateMapper.visionNormalizedRectToVideoPixels(
                    detection.normalizedRect,
                    videoWidth: videoWidth,
                    videoHeight: videoHeight
                )
                return (rect, detection.confidence)
            }
            let score = RegionFramePicker.subtitlePresenceScore(
                candidates: scored,
                videoHeight: videoHeight
            )

            if score > bestScore {
                bestScore = score
                bestDetections = detections
                bestSeconds = seconds
            }

            if score >= RegionFramePicker.earlyStopScore {
                break
            }
        }

        guard let detections = bestDetections, let usedSeconds = bestSeconds else {
            if attempts == 0 {
                status = .failed("无法抽取代表帧")
            } else if case .performFailed(let message)? = lastError {
                status = .failed("Vision 检测失败: \(message)")
            } else {
                status = .failed("多帧均未检测到文字候选框，可拖到有字幕处后点「重检当前帧」")
            }
            return
        }

        sampleSecondsUsed = usedSeconds
        applyDetections(detections, videoWidth: videoWidth, videoHeight: videoHeight)
        status = .ready
    }

    /// 对指定时间点单帧检测（用于「当前播放头重检」）。
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

        sampleSecondsUsed = seconds
        applyDetections(detections, videoWidth: videoWidth, videoHeight: videoHeight)
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

    private func applyDetections(
        _ detections: [VisionTextDetector.Detection],
        videoWidth: Int,
        videoHeight: Int
    ) {
        var built: [TextBoxCandidate] = []
        built.reserveCapacity(detections.count)

        for (index, detection) in detections.enumerated() {
            let pixelRect = VideoCoordinateMapper.visionNormalizedRectToVideoPixels(
                detection.normalizedRect,
                videoWidth: videoWidth,
                videoHeight: videoHeight
            )
            let preview = detection.text.isEmpty ? "（空）" : detection.text
            built.append(
                TextBoxCandidate(
                    id: index + 1,
                    pixelRect: pixelRect,
                    textPreview: preview,
                    confidence: detection.confidence,
                    colorIndex: index
                )
            )
        }

        candidates = built

        // 仅预选下部带；无下部候选时留空，由用户点选（避免整屏全选撑大 region）
        selectedIds = RegionMerger.autoSelectIds(
            candidates: built.map { ($0.id, $0.pixelRect, $0.confidence) },
            videoHeight: videoHeight
        )
        refreshMergedRegion()
    }

    private func refreshMergedRegion() {
        mergedRegion = RegionMerger.mergedFullWidthRegion(
            candidates: candidates.map { ($0.id, $0.pixelRect) },
            selectedIds: selectedIds,
            videoWidth: videoWidth,
            videoHeight: videoHeight
        )
    }

    /// feat-022b：供 `start_job` 使用的 `region_box`；无合并区域时返回 nil。
    func regionBoxForIPC() -> RegionBox? {
        RegionMerger.regionBoxFromMergedRegion(
            mergedRegion,
            videoWidth: videoWidth,
            videoHeight: videoHeight
        )
    }

    /// feat-034b：供 `start_job` 使用的 `subtitle_profile`（相对 region crop）。
    func subtitleProfileForIPC() -> SubtitleProfilePayload? {
        guard let regionBox = regionBoxForIPC() else { return nil }
        return RegionMerger.subtitleProfileFromSelection(
            candidates: candidates.map { ($0.id, $0.pixelRect, $0.textPreview) },
            selectedIds: selectedIds,
            regionBox: regionBox
        )
    }
}
