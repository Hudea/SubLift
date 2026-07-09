import AVFoundation
import CoreMedia
import Foundation

/// 协调 PipelineClient（IPC）的端到端字幕提取。
///
/// **统一抽帧（path mode）**：Swift 只传 `video_path` + 参数，Python 用
/// `FfmpegExtractor` 抽帧（与 CLI / benchmark 同源），不再 AVF+JPEG 推帧。
@MainActor
final class SubtitleExtractor: ObservableObject {

    enum Status: Equatable {
        case idle
        case startingServer
        case sampling(progress: Double, frameCount: Int, totalFrames: Int)
        case processing
        case done(entryCount: Int)
        case error(String)
    }

    @Published private(set) var status: Status = .idle
    @Published private(set) var entries: [SubtitleEntryData] = []

    private var client = PipelineClient()
    private var currentTask: Task<Void, Never>?
    private var _cancelled = false

    var isRunning: Bool {
        switch status {
        case .idle, .done, .error:
            return false
        default:
            return true
        }
    }

    func extract(
        videoURL: URL,
        fps: Int = 5,
        engine: OcrEngineName = .vision,
        regionBox: RegionBox? = nil,
        enableSsimPatrol: Bool = false
    ) {
        guard !isRunning else { return }
        _cancelled = false
        entries = []
        currentTask = Task { [weak self] in
            await self?.runExtract(
                videoURL: videoURL,
                fps: fps,
                engine: engine,
                regionBox: regionBox,
                enableSsimPatrol: enableSsimPatrol
            )
        }
    }

    func cancel() {
        _cancelled = true
        // 只关 socket 打断阻塞读；不要 Task.cancel()，否则日志会误报「cancelled」
        // 而真实原因可能是进度未刷新导致用户误点取消。
        let client = self.client
        DispatchQueue.global(qos: .userInitiated).async {
            client.stop()
        }
        status = .error("已取消")
        print("[SubLift] extract cancel requested (closing IPC socket)")
    }

    // MARK: - Private

    private func runExtract(
        videoURL: URL,
        fps: Int,
        engine: OcrEngineName,
        regionBox: RegionBox?,
        enableSsimPatrol: Bool
    ) async {
        do {
            let startTime = Date()

            status = .startingServer
            _ = try await runOnBackground { [client] in
                try client.start(engine: engine.rawValue)
            }
            defer {
                let client = self.client
                DispatchQueue.global(qos: .utility).async {
                    client.stop()
                }
            }

            if _cancelled { throw PipelineClientError.connectionClosed }

            let durationMs = await estimateDurationMs(url: videoURL)
            let totalFrames = await FrameSampler.estimateFrameCount(url: videoURL, fps: fps)
            let totalFramesSafe = max(1, totalFrames)
            let videoId = UUID().uuidString
            let confidenceThreshold = 0.5
            let patrolPayload: Bool? = enableSsimPatrol ? true : nil
            let videoPath = videoURL.standardizedFileURL.path

            let startMsg = StartJobMessage(
                videoId: videoId,
                fps: Double(fps),
                engine: engine,
                confidenceThreshold: confidenceThreshold,
                regionBox: regionBox,
                durationMs: durationMs,
                enableSsimPatrol: patrolPayload,
                videoPath: videoPath
            )
            Self.logStartJob(
                videoURL: videoURL,
                videoId: videoId,
                videoPath: videoPath,
                fps: fps,
                engine: engine,
                confidenceThreshold: confidenceThreshold,
                regionBox: regionBox,
                durationMs: durationMs,
                uiPatrolToggle: enableSsimPatrol,
                patrolPayload: patrolPayload,
                estimatedFrames: totalFramesSafe
            )

            // 立刻显示进度条，避免一直 0 像未启动
            status = .sampling(progress: 0, frameCount: 0, totalFrames: totalFramesSafe)

            let response = try await runOnBackground { [client] in
                try client.requestStreaming(
                    startMsg,
                    expecting: EntriesMessage.self,
                    onPushEntry: { entry in
                        Task { @MainActor [weak self] in
                            self?.entries.append(entry)
                        }
                    },
                    onProgress: { pct, _ in
                        Task { @MainActor [weak self] in
                            guard let self else { return }
                            let frames = max(0, Int((pct * Double(totalFramesSafe)).rounded(.down)))
                            self.status = .sampling(
                                progress: min(max(pct, 0), 1),
                                frameCount: min(frames, totalFramesSafe),
                                totalFrames: totalFramesSafe
                            )
                        }
                    }
                )
            }

            if _cancelled {
                print("[SubLift] extract cancelled after IPC")
                return
            }

            let resultEntries = response.entries
            self.entries = resultEntries

            let elapsed = Date().timeIntervalSince(startTime)
            let emptyCount = resultEntries.filter {
                $0.text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
            }.count
            print(
                """
                [SubLift] extract done (path mode / FfmpegExtractor)
                  elapsed_s=\(String(format: "%.2f", elapsed))
                  estimated_frames=\(totalFramesSafe)
                  entries=\(resultEntries.count) empty_text=\(emptyCount)
                  region_box=\(Self.formatRegionBox(regionBox))
                  video_path=\(videoPath)
                """
            )

            status = .done(entryCount: resultEntries.count)

        } catch PipelineClientError.connectionClosed {
            if _cancelled {
                status = .error("已取消")
                print("[SubLift] extract cancelled (socket closed)")
            } else {
                status = .error("连接中断")
                print("[SubLift] extract connection closed unexpectedly")
            }
        } catch is CancellationError {
            status = .error("已取消")
            print("[SubLift] extract cancelled (task cancellation)")
        } catch {
            if _cancelled {
                status = .error("已取消")
                print("[SubLift] extract cancelled: \(error)")
            } else {
                status = .error("提取失败: \(error.localizedDescription)")
                print("[SubLift] extract error: \(error)")
            }
        }
    }

    private static func logStartJob(
        videoURL: URL,
        videoId: String,
        videoPath: String,
        fps: Int,
        engine: OcrEngineName,
        confidenceThreshold: Double,
        regionBox: RegionBox?,
        durationMs: Int,
        uiPatrolToggle: Bool,
        patrolPayload: Bool?,
        estimatedFrames: Int
    ) {
        let regionStr = formatRegionBox(regionBox)
        let matchesFeat033: String
        if let regionBox, regionBox.count == 4 {
            let same = regionBox[0] == 0 && regionBox[1] == 848
                && regionBox[2] == 1920 && regionBox[3] == 87
            matchesFeat033 = same ? "YES" : "NO"
        } else {
            matchesFeat033 = "N/A (nil → bottom_crop)"
        }
        let patrolPayloadStr = patrolPayload.map { $0 ? "true" : "false" } ?? "nil"
        print(
            """
            [SubLift] start_job (Swift → Python path mode)
              video=\(videoURL.lastPathComponent)
              video_path=\(videoPath)
              video_id=\(videoId)
              fps=\(fps) engine=\(engine.rawValue) confidence_threshold=\(confidenceThreshold)
              duration_ms=\(durationMs) estimated_frames=\(estimatedFrames)
              region_box=\(regionStr)
              region_matches_feat033_[0,848,1920,87]=\(matchesFeat033)
              enable_ssim_patrol UI=\(uiPatrolToggle) payload=\(patrolPayloadStr)
              frame_path=Python FfmpegExtractor (same as CLI/benchmark)
            """
        )
    }

    private static func formatRegionBox(_ box: RegionBox?) -> String {
        guard let box else { return "nil" }
        return "[\(box.map(String.init).joined(separator: ", "))]"
    }

    private func estimateDurationMs(url: URL) async -> Int {
        if url.pathExtension.lowercased() == "mkv" {
            let frames = await FrameSampler.estimateFrameCount(url: url, fps: 1)
            return frames * 1000
        }
        let asset = AVURLAsset(url: url)
        let cmDuration = try? await asset.load(.duration)
        guard let cmDuration = cmDuration else { return 0 }
        let secs = CMTimeGetSeconds(cmDuration)
        return secs.isFinite ? Int(secs * 1000) : 0
    }

    /// 在后台队列跑阻塞 IPC，且 **不** 因父 Task.cancel 误抛 CancellationError。
    private func runOnBackground<T: Sendable>(
        _ body: @escaping @Sendable () throws -> T
    ) async throws -> T {
        try await withCheckedThrowingContinuation { cont in
            DispatchQueue.global(qos: .userInitiated).async {
                do {
                    cont.resume(returning: try body())
                } catch {
                    cont.resume(throwing: error)
                }
            }
        }
    }
}
