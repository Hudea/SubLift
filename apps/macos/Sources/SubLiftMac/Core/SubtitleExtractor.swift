import AVFoundation
import CoreMedia
import Foundation

/// 协调 PipelineClient（IPC）的端到端字幕提取。
///
/// **统一抽帧（path mode）**：Swift 只传 `video_path` + 参数，由实际选中的
/// Native Worker 使用 C++ `FfmpegExtractor` 抽帧，不再 AVF+JPEG 推帧。
///
/// **生命周期**：每次 `extract` 独占一个 `PipelineClient` + `jobToken`。
/// 取消/完成后丢弃 token，迟到的 progress / push_entry 不会污染 UI 状态。
@MainActor
final class SubtitleExtractor: ObservableObject {

    enum Status: Equatable {
        case idle
        case startingServer
        case processing(progress: Double, frameCount: Int, totalFrames: Int)
        case finalizing
        case done(entryCount: Int)
        case error(String)
    }

    @Published private(set) var status: Status = .idle
    @Published private(set) var entries: [SubtitleEntryData] = []
    /// 已处理视频时长 / 实际处理时间；仅在 processing 阶段有有效值。
    @Published private(set) var processingRate: Double?
    /// 实际运行时身份，避免 UI 把默认路由或 fallback 隐藏起来。
    @Published private(set) var runtimeIdentity: String?

    /// 当前运行任务的 token；`nil` 表示无活跃任务（含已取消/已完成）。
    private var jobToken: UUID?
    /// 当前任务独占的 IPC 客户端（取消时只 stop 此实例）。
    private var activeClient: PipelineClient?
    /// 是否仍接受流式 progress / push_entry（最终 entries 落地或取消后为 false）。
    private var acceptingLiveUpdates = false
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
        regionBox: RegionBox? = nil,
        subtitleProfile: SubtitleProfilePayload? = nil
    ) {
        guard !isRunning else { return }

        let token = UUID()
        jobToken = token
        acceptingLiveUpdates = true
        entries = []
        processingRate = nil
        runtimeIdentity = nil

        // 每任务独占客户端，避免取消 teardown 关掉新任务的 socket/进程。
        let client = PipelineClient()
        activeClient = client

        currentTask = Task { [weak self] in
            await self?.runExtract(
                videoURL: videoURL,
                fps: fps,
                engine: engine,
                regionBox: regionBox,
                subtitleProfile: subtitleProfile,
                jobToken: token,
                client: client
            )
        }
    }

    func cancel() {
        guard isRunning else { return }

        // 立刻失效 token / 流式更新，后续迟到回调全部丢弃。
        let client = activeClient
        jobToken = nil
        acceptingLiveUpdates = false
        activeClient = nil
        status = .error("已取消")
        processingRate = nil

        // 只关本任务 socket；不要 Task.cancel()，否则日志会误报 cancelled。
        if let client {
            DispatchQueue.global(qos: .userInitiated).async {
                client.stop()
            }
        }
        print("[SubLift] extract cancel requested (closing IPC socket)")
    }

    // MARK: - Private

    private func runExtract(
        videoURL: URL,
        fps: Int,
        engine: OcrEngineName,
        regionBox: RegionBox?,
        subtitleProfile: SubtitleProfilePayload?,
        jobToken token: UUID,
        client: PipelineClient
    ) async {
        defer {
            // 任务结束时只 stop 自己的 client；若 cancel 已 swap 掉 activeClient 则仍安全。
            if activeClient === client {
                activeClient = nil
            }
            if jobToken == token {
                jobToken = nil
                acceptingLiveUpdates = false
            }
            DispatchQueue.global(qos: .utility).async {
                client.stop()
            }
        }

        do {
            let startTime = Date()
            var processingStartedAt: Date?

            guard isCurrentJob(token) else { return }
            status = .startingServer
            _ = try await runOnBackground {
                try client.start(engine: engine.rawValue)
            }

            guard isCurrentJob(token) else { return }
            let workerChoice = client.lastWorkerChoice
            if let choice = workerChoice {
                runtimeIdentity = Self.formatRuntimeIdentity(choice)
            }

            let durationMs = await estimateDurationMs(url: videoURL)
            let totalFrames = await FrameSampler.estimateFrameCount(url: videoURL, fps: fps)
            let totalFramesSafe = max(1, totalFrames)
            let videoId = UUID().uuidString
            let confidenceThreshold = 0.5
            let videoPath = videoURL.standardizedFileURL.path

            // SSIM patrol 是 Worker 内部默认机制（Config.enable_ssim_patrol=True）。
            // GUI 不传 enable_ssim_patrol，统一走所选 runtime 的默认；IPC 字段仍保留
            // 供 benchmark / 回归 / 内部诊断显式关闭。
            let startMsg = StartJobMessage(
                videoId: videoId,
                fps: Double(fps),
                engine: engine,
                confidenceThreshold: confidenceThreshold,
                regionBox: regionBox,
                durationMs: durationMs,
                videoPath: videoPath,
                subtitleProfile: subtitleProfile
            )
            Self.logStartJob(
                videoURL: videoURL,
                videoId: videoId,
                videoPath: videoPath,
                fps: fps,
                engine: engine,
                confidenceThreshold: confidenceThreshold,
                regionBox: regionBox,
                subtitleProfile: subtitleProfile,
                durationMs: durationMs,
                estimatedFrames: totalFramesSafe,
                workerChoice: workerChoice
            )

            guard isCurrentJob(token) else { return }
            // 立刻显示进度条，避免一直 0 像未启动
            status = .processing(progress: 0, frameCount: 0, totalFrames: totalFramesSafe)

            let response = try await runOnBackground {
                try client.requestStreaming(
                    startMsg,
                    expecting: EntriesMessage.self,
                    onPushEntry: { [weak self] entry in
                        Task { @MainActor [weak self] in
                            guard let self, self.shouldAcceptLiveUpdate(token) else { return }
                            self.entries.append(entry)
                        }
                    },
                    onProgress: { [weak self] pct, stage in
                        Task { @MainActor [weak self] in
                            guard let self, self.shouldAcceptLiveUpdate(token) else { return }
                            if stage == "finalizing" {
                                self.processingRate = nil
                                self.status = .finalizing
                            } else {
                                let processingStart = processingStartedAt ?? Date()
                                processingStartedAt = processingStart
                                let frames = max(0, Int((pct * Double(totalFramesSafe)).rounded(.down)))
                                self.processingRate = ProcessingRate.realTimeMultiplier(
                                    processedFrames: frames,
                                    sampleFps: fps,
                                    elapsedSeconds: Date().timeIntervalSince(processingStart)
                                )
                                self.status = .processing(
                                    progress: min(max(pct, 0), 1),
                                    frameCount: min(frames, totalFramesSafe),
                                    totalFrames: totalFramesSafe
                                )
                            }
                        }
                    }
                )
            }

            guard isCurrentJob(token) else {
                print("[SubLift] extract cancelled after IPC")
                return
            }

            // 先关闭流式入口，再写最终列表，避免迟到 push_entry 追加到 deduped 结果。
            acceptingLiveUpdates = false
            let resultEntries = response.entries
            self.entries = resultEntries
            self.processingRate = nil

            let elapsed = Date().timeIntervalSince(startTime)
            let emptyCount = resultEntries.filter {
                $0.text.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
            }.count
            let pathFields = Self.pathModeLogFields(for: workerChoice)
            let runtime = workerChoice?.runtime.rawValue ?? "unknown"
            let resolvedVia = workerChoice?.resolvedVia.rawValue ?? "unknown"
            print(
                """
                [SubLift] extract done (Swift → \(pathFields.backend) worker path mode)
                  elapsed_s=\(String(format: "%.2f", elapsed))
                  runtime=\(runtime) engine=\(engine.rawValue) resolved_via=\(resolvedVia)
                  frame_path=\(pathFields.extractor)
                  estimated_frames=\(totalFramesSafe)
                  entries=\(resultEntries.count) empty_text=\(emptyCount)
                  region_box=\(Self.formatRegionBox(regionBox))
                  video_path=\(videoPath)
                """
            )

            status = .done(entryCount: resultEntries.count)

        } catch PipelineClientError.connectionClosed {
            // cancel() 已失效 token 并写好状态；此处仅处理「意外断连」。
            if isCurrentJob(token) {
                processingRate = nil
                status = .error("连接中断")
                print("[SubLift] extract connection closed unexpectedly")
            } else {
                print("[SubLift] extract cancelled (socket closed)")
            }
        } catch is CancellationError {
            if isCurrentJob(token) {
                processingRate = nil
                status = .error("已取消")
            }
            print("[SubLift] extract cancelled (task cancellation)")
        } catch {
            if isCurrentJob(token) {
                processingRate = nil
                status = .error("提取失败: \(error.localizedDescription)")
                print("[SubLift] extract error: \(error)")
            } else {
                print("[SubLift] extract error ignored (stale job): \(error)")
            }
        }
    }

    /// 任务仍是当前 job（未被 cancel / 未被新 extract 替换）。
    private func isCurrentJob(_ token: UUID) -> Bool {
        jobToken == token
    }

    /// 允许落地 progress / push_entry。
    private func shouldAcceptLiveUpdate(_ token: UUID) -> Bool {
        acceptingLiveUpdates && jobToken == token
    }

    private static func logStartJob(
        videoURL: URL,
        videoId: String,
        videoPath: String,
        fps: Int,
        engine: OcrEngineName,
        confidenceThreshold: Double,
        regionBox: RegionBox?,
        subtitleProfile: SubtitleProfilePayload?,
        durationMs: Int,
        estimatedFrames: Int,
        workerChoice: WorkerChoice?
    ) {
        let regionStr = formatRegionBox(regionBox)
        let pathFields = pathModeLogFields(for: workerChoice)
        let runtime = workerChoice?.runtime.rawValue ?? "unknown"
        let resolvedVia = workerChoice?.resolvedVia.rawValue ?? "unknown"
        let matchesFeat033: String
        if let regionBox, regionBox.count == 4 {
            let same = regionBox[0] == 0 && regionBox[1] == 848
                && regionBox[2] == 1920 && regionBox[3] == 87
            matchesFeat033 = same ? "YES" : "NO"
        } else {
            matchesFeat033 = "N/A (nil → bottom_crop)"
        }
        let profileStr: String
        if let p = subtitleProfile {
            profileStr =
                "script=\(p.script) center=(\(p.centerX),\(p.centerY)) "
                + "height=\(p.height) y=[\(p.yMin),\(p.yMax)]"
        } else {
            profileStr = "nil"
        }
        print(
            """
            [SubLift] start_job (Swift → \(pathFields.backend) worker path mode)
              video=\(videoURL.lastPathComponent)
              video_path=\(videoPath)
              video_id=\(videoId)
              fps=\(fps) engine=\(engine.rawValue) runtime=\(runtime) resolved_via=\(resolvedVia)
              confidence_threshold=\(confidenceThreshold)
              duration_ms=\(durationMs) estimated_frames=\(estimatedFrames)
              region_box=\(regionStr)
              region_matches_feat033_[0,848,1920,87]=\(matchesFeat033)
              subtitle_profile=\(profileStr)
              enable_ssim_patrol=omitted (backend default True)
              frame_path=\(pathFields.extractor) (worker-owned path mode)
            """
        )
    }

    nonisolated static func pathModeLogFields(
        for choice: WorkerChoice?
    ) -> (backend: String, extractor: String) {
        switch choice?.runtime {
        case .cpp:
            return ("C++", "C++ FfmpegExtractor")
        case nil:
            return ("unknown", "unknown extractor")
        }
    }

    private static func formatRegionBox(_ box: RegionBox?) -> String {
        guard let box else { return "nil" }
        return "[\(box.map(String.init).joined(separator: ", "))]"
    }

    private static func formatRuntimeIdentity(_ choice: WorkerChoice) -> String {
        guard choice.engine == .paddle else {
            return "\(choice.engine.rawValue) · \(choice.runtime.rawValue)"
        }
        return "PaddleOCR · cpp · PP-OCRv6 small · stable"
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
