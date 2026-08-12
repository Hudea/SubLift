import Foundation

/// 08206：Batch Runner 的 IPC 抽象（PipelineClient 最小 conform；测试注入 Fake）。
protocol BatchIPCClient: AnyObject, Sendable {
    func startBatch(engine: String) throws
    func stop()
    var lastWorkerChoice: WorkerChoice? { get }
    /// 阻塞式流式请求；onProgress 报告 (pct 0-1, stage)。
    func requestBatchStreaming(
        _ message: StartJobMessage,
        onProgress: @escaping @Sendable (Double, String) -> Void
    ) throws -> EntriesMessage
}

/// 08206：生产 Batch Runner——每任务独占 PipelineClient/Worker/socket。
///
/// - 独占生命周期：run 内创建 client，defer stop()（teardown 关闭 socket）；
///   Workspace 与 Batch 不共享可变 SubtitleExtractor。
/// - 事件映射（Scheduler 不解析 UDS）：
///   start → .preparing；onProgress（非 finalizing）→ .extracting + pct；
///   onProgress stage=="finalizing" → .exporting；final entries 原子写出成功 → .completed。
/// - 隐藏 Mock 启动前再次归一化（non-dev → vision）。
/// - 写出后释放完整 entries：outcome 只保留 entryCount/outputURL/runtimeIdentity 摘要。
final class BatchExtractionRunner: BatchTaskRunning, @unchecked Sendable {

    private let clientFactory: @Sendable () -> any BatchIPCClient

    init(clientFactory: @escaping @Sendable () -> any BatchIPCClient = { PipelineClient() }) {
        self.clientFactory = clientFactory
    }

    func run(
        _ task: BatchTask,
        onUpdate: @escaping @Sendable (BatchTaskStatus, Double?) -> Void
    ) async -> BatchRunOutcome {
        // fail-closed：源文件不存在/不可读 → 快速 failed（不启动 Worker；
        // scanner 正常路径不会产生，但持久化恢复/手动构造可能）。
        let fileManager = FileManager.default
        guard fileManager.fileExists(atPath: task.sourceURL.path),
              fileManager.isReadableFile(atPath: task.sourceURL.path) else {
            return .failed("源文件不存在或不可读")
        }

        let client = clientFactory()
        defer {
            // teardown：关闭本任务独占 socket（不迟到的资源回收；幂等）。
            client.stop()
        }

        // 隐藏 Mock 启动前再次归一化（与请求路径同一策略）。
        let engine = EngineCapability.normalizedSelection(task.configuration.engine, developerMode: false)

        do {
            onUpdate(.preparing, nil)
            try await runBlocking(client: client) {
                try client.startBatch(engine: engine.rawValue)
            }
            guard !Task.isCancelled else { return .cancelled }

            let runtimeIdentity = client.lastWorkerChoice?.runtime.rawValue
            let durationMs = FfmpegFrameSampler.probeDurationMs(url: task.sourceURL)

            let startMsg = StartJobMessage(
                videoId: UUID().uuidString,
                fps: 5,
                engine: engine,
                confidenceThreshold: 0.5,
                regionBox: nil,
                durationMs: durationMs,
                videoPath: task.sourceURL.standardizedFileURL.path,
                subtitleProfile: nil
            )

            onUpdate(.extracting, 0)
            let response = try await runBlocking(client: client) {
                try client.requestBatchStreaming(startMsg) { pct, stage in
                    // 真实进度映射：非 finalizing → extracting + pct；finalizing → exporting。
                    if stage == "finalizing" {
                        onUpdate(.exporting, nil)
                    } else {
                        onUpdate(.extracting, min(max(pct, 0), 1))
                    }
                }
            }
            guard !Task.isCancelled else { return .cancelled }

            // finalizing → exporting（写出阶段）。
            onUpdate(.exporting, nil)

            let srt = try SrtFormatter.format(entries: response.entries)
            let outputURL = task.outputURL ?? defaultSidecarURL(for: task.sourceURL)
            try AtomicSrtWriter.write(srt, to: outputURL)

            // 写出成功后才 completed；只保留摘要（entries 已释放）。
            return .completed(
                entryCount: response.entries.count,
                outputURL: outputURL,
                runtimeIdentity: runtimeIdentity
            )
        } catch PipelineClientError.connectionClosed {
            if Task.isCancelled { return .cancelled }
            return .failed("连接中断")
        } catch is CancellationError {
            return .cancelled
        } catch {
            return .failed("提取失败: \(error.localizedDescription)")
        }
    }

    /// sidecar 默认输出（outputURL 未规划时）。
    private func defaultSidecarURL(for sourceURL: URL) -> URL {
        sourceURL.deletingPathExtension().appendingPathExtension("srt")
    }

    /// 阻塞操作包装：取消时主动关闭 client（中断阻塞读循环 → connectionClosed），
    /// 保证 continuation 必然 resume（无泄漏、worker 资源及时回收）。
    /// 注意：withCheckedThrowingContinuation 不会因任务取消自动 resume，
    /// 由 onCancel 的 client.stop() 提供中断源。
    private func runBlocking<T: Sendable>(
        client: any BatchIPCClient,
        _ operation: @escaping @Sendable () throws -> T
    ) async throws -> T {
        try await withTaskCancellationHandler {
            try await withCheckedThrowingContinuation { continuation in
                DispatchQueue.global(qos: .userInitiated).async {
                    do {
                        continuation.resume(returning: try operation())
                    } catch {
                        continuation.resume(throwing: error)
                    }
                }
            }
        } onCancel: {
            // 中断阻塞读（socket 关闭 → recv 返回 → connectionClosed → continuation resume）。
            client.stop()
        }
    }
}
