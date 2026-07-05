import AVFoundation
import CoreMedia
import Foundation

/// feat-018：协调 FrameSampler（抽帧）+ PipelineClient（IPC）的端到端字幕提取。
///
/// 流式抽帧 + 批量 OCR：Swift 边抽帧边发 frame，Python 缓冲，
/// finalize 后一次性跑 Pipeline（feat-016 batch 模型不变）。
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
        regionBox: RegionBox? = nil
    ) {
        guard !isRunning else { return }
        currentTask = Task { [weak self] in
            await self?.runExtract(
                videoURL: videoURL,
                fps: fps,
                engine: engine,
                regionBox: regionBox
            )
        }
    }

    func cancel() {
        currentTask?.cancel()
        Task.detached { [client] in client.stop() }
        status = .idle
    }

    // MARK: - Private

    private func runExtract(
        videoURL: URL,
        fps: Int,
        engine: OcrEngineName,
        regionBox: RegionBox?
    ) async {
        do {
            let startTime = Date()

            status = .startingServer
            // IPC 调用是同步阻塞的，放到 detached task 里跑，让 main actor 能刷新 UI
            _ = try await runDetached { [client] in
                try client.start(engine: engine.rawValue)
            }
            defer {
                let client = self.client
                Task.detached { client.stop() }
            }

            let durationMs = await estimateDurationMs(url: videoURL)
            let totalFrames = await FrameSampler.estimateFrameCount(url: videoURL, fps: fps)
            let totalFramesSafe = max(1, totalFrames)
            let videoId = UUID().uuidString

            // 1. start_job
            let startMsg = StartJobMessage(
                videoId: videoId,
                fps: Double(fps),
                engine: engine,
                confidenceThreshold: 0.5,
                regionBox: regionBox,
                durationMs: durationMs
            )
            _ = try await runDetached { [client] in
                try client.request(startMsg, expecting: ProgressMessage.self)
            }

            // 2. 流式抽帧 + 发 frame
            let config = FrameSampler.Config(fps: fps)
            var frameCount = 0
            for await (frame, error) in FrameSampler.sample(url: videoURL, config: config) {
                if let error = error { throw error }
                guard let frame = frame else { continue }
                try Task.checkCancellation()

                let base64 = frame.jpegData.base64EncodedString()
                let frameMsg = FrameMessage(
                    videoId: videoId,
                    tsMs: frame.tsMs,
                    jpegBytes: base64,
                    regionBox: nil
                )
                _ = try await runDetached { [client] in
                    try client.request(frameMsg, expecting: ProgressMessage.self)
                }
                frameCount += 1
                let pct = min(Double(frameCount) / Double(totalFramesSafe), 1.0)
                status = .sampling(
                    progress: pct,
                    frameCount: frameCount,
                    totalFrames: totalFramesSafe
                )
            }

            // 3. finalize
            status = .processing
            let finalizeMsg = FinalizeMessage(videoId: videoId)
            let response = try await runDetached { [client] in
                try client.request(finalizeMsg, expecting: EntriesMessage.self)
            }

            // 4. 拿到 entries
            let resultEntries = response?.entries ?? []
            self.entries = resultEntries

            let elapsed = Date().timeIntervalSince(startTime)
            #if DEBUG
            print("[feat-018] 端到端耗时: \(String(format: "%.2f", elapsed))s, 帧数: \(frameCount), entries: \(resultEntries.count)")
            #endif

            status = .done(entryCount: resultEntries.count)

        } catch is CancellationError {
            status = .error("已取消")
        } catch {
            status = .error("提取失败: \(error.localizedDescription)")
            #if DEBUG
            print("[feat-018] 错误: \(error)")
            #endif
        }
    }

    private func estimateDurationMs(url: URL) async -> Int {
        if url.pathExtension.lowercased() == "mkv" {
            // mkv 用 ffprobe 取时长（AVURLAsset.duration 对 mkv 不可靠）
            let frames = await FrameSampler.estimateFrameCount(url: url, fps: 1)
            return frames * 1000
        }
        let asset = AVURLAsset(url: url)
        let cmDuration = try? await asset.load(.duration)
        guard let cmDuration = cmDuration else { return 0 }
        let secs = CMTimeGetSeconds(cmDuration)
        return secs.isFinite ? Int(secs * 1000) : 0
    }

    /// 把同步阻塞的 IPC 调用扔到后台线程跑，让 main actor 在 await 期间能刷新 UI。
    private func runDetached<T>(_ body: @escaping () throws -> T) async throws -> T {
        try await Task.detached(priority: .userInitiated) { try body() }.value
    }
}
