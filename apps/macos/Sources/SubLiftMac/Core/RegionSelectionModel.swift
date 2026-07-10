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

    private var videoWidth: Int = 0
    private var videoHeight: Int = 0

    func clear() {
        status = .idle
        candidates = []
        selectedIds = []
        mergedRegion = nil
        videoWidth = 0
        videoHeight = 0
    }

    /// 对代表帧执行 Vision 检测并生成候选框。
    func detect(url: URL, videoWidth: Int, videoHeight: Int, sampleSeconds: Double) async {
        clear()
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

        let detections: [VisionTextDetector.Detection]
        do {
            detections = try await Task.detached(priority: .userInitiated) {
                try VisionTextDetector.detect(in: cgImage)
            }.value
        } catch let error as VisionTextDetector.DetectError {
            switch error {
            case .noResults:
                status = .failed("未检测到文字候选框")
            case .performFailed(let message):
                status = .failed("Vision 检测失败: \(message)")
            }
            return
        } catch {
            status = .failed("Vision 检测失败: \(error.localizedDescription)")
            return
        }

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

        let autoIds = RegionMerger.autoSelectIds(
            candidates: built.map { ($0.id, $0.pixelRect, $0.confidence) },
            videoHeight: videoHeight
        )
        selectedIds = autoIds.isEmpty ? Set(built.map(\.id)) : autoIds
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
            candidates: candidates.map { ($0.id, $0.pixelRect) },
            selectedIds: selectedIds,
            regionBox: regionBox
        )
    }
}