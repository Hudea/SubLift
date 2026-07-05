import AVFoundation
import CoreImage
import CoreMedia
import Foundation
import ImageIO

/// feat-018：AVFoundation 抽帧 + JPEG 编码。
///
/// 用 AVAssetReader 按指定 fps 跳采样，CVPixelBuffer → CGImage → JPEG q=85。
/// 输出 (ts_ms, jpeg_bytes) 流，供 PipelineClient 发送 FrameMessage。
enum FrameSampler {

    /// 抽帧参数。
    struct Config: Equatable {
        var fps: Int = 5
        var jpegQuality: CGFloat = 0.85

        static let `default` = Config()
    }

    /// 单帧采样结果。
    struct SampledFrame: Equatable {
        let tsMs: Int
        let jpegData: Data
    }

    /// 抽帧错误。
    enum SampleError: Error, Equatable {
        case noVideoTrack
        case readerStartFailed(String)
        case encodeJPEGFailed
    }

    /// 抽取指定时间点的单帧（用于预览检测 / 静态预览）。
    static func captureFrameAt(url: URL, seconds: Double) async -> CGImage? {
        if url.pathExtension.lowercased() == "mkv" {
            return FfmpegFrameSampler.captureFrameAt(url: url, seconds: seconds)
                .flatMap { $0.cgImage(forProposedRect: nil, context: nil, hints: nil) }
        }
        return await captureFrameWithAVFoundation(url: url, seconds: seconds)
    }

    /// 估算总帧数（用于进度条）。按扩展名路由。
    static func estimateFrameCount(url: URL, fps: Int) async -> Int {
        if url.pathExtension.lowercased() == "mkv" {
            return await FfmpegFrameSampler.estimateFrameCount(url: url, fps: fps)
        }
        let asset = AVURLAsset(url: url)
        let cmDuration = try? await asset.load(.duration)
        guard let cmDuration = cmDuration else { return 0 }
        let duration = CMTimeGetSeconds(cmDuration)
        guard duration.isFinite, duration > 0 else { return 0 }
        return Int(duration * Double(fps))
    }

    /// 抽帧入口：按文件扩展名路由到 AVFoundation 或 ffmpeg。
    ///
    /// - .mkv → FfmpegFrameSampler（AVFoundation 不支持 mkv 容器）
    /// - 其他 → AVAssetReader（mp4/mov 全支持）
    /// - Returns: (SampledFrame?, Error?) —— error 非 nil 时表示失败，frame 为 nil 且 error 为 nil 时表示结束
    static func sample(url: URL, config: Config = .default) -> AsyncStream<(SampledFrame?, Error?)> {
        if url.pathExtension.lowercased() == "mkv" {
            return FfmpegFrameSampler.sample(url: url, config: config)
        }
        return sampleWithAVAssetReader(url: url, config: config)
    }

    /// 用 AVAssetReader 抽帧，返回 AsyncStream。
    ///
    /// 基于 PTS 跳采样：每帧 PTS ≥ nextTargetMs 时取，然后 nextTargetMs += 1000/fps。
    static func sampleWithAVAssetReader(
        url: URL,
        config: Config = .default
    ) -> AsyncStream<(SampledFrame?, Error?)> {
        AsyncStream { continuation in
            Task.detached(priority: .userInitiated) {
                do {
                    try await sampleImpl(url: url, config: config, continuation: continuation)
                } catch {
                    continuation.yield((nil, error))
                    continuation.finish()
                    return
                }
                continuation.finish()
            }
        }
    }

    // MARK: - JPEG 编码（纯函数，便于单测）

    /// 把 CGImage 编码为 JPEG Data。
    /// - Parameters:
    ///   - cgImage: 输入图像
    ///   - quality: JPEG 质量（0.0~1.0，默认 0.85）
    /// - Returns: JPEG 字节；编码失败返回 nil
    static func encodeJPEG(_ cgImage: CGImage, quality: CGFloat = 0.85) -> Data? {
        let mutableData = NSMutableData()
        guard let destination = CGImageDestinationCreateWithData(
            mutableData,
            "public.jpeg" as CFString,
            1,
            nil
        ) else { return nil }
        CGImageDestinationAddImage(destination, cgImage, [
            kCGImageDestinationLossyCompressionQuality: quality
        ] as CFDictionary)
        guard CGImageDestinationFinalize(destination) else { return nil }
        return mutableData as Data
    }

    // MARK: - AVFoundation 单帧

    private static func captureFrameWithAVFoundation(url: URL, seconds: Double) async -> CGImage? {
        await withCheckedContinuation { continuation in
            Task.detached(priority: .userInitiated) {
                let asset = AVURLAsset(url: url)
                let generator = AVAssetImageGenerator(asset: asset)
                generator.appliesPreferredTrackTransform = true
                let time = CMTime(seconds: max(0, seconds), preferredTimescale: 600)
                generator.generateCGImagesAsynchronously(forTimes: [NSValue(time: time)]) {
                    _, image, _, _, _ in
                    continuation.resume(returning: image)
                }
            }
        }
    }

    // MARK: - Private

    private static func sampleImpl(
        url: URL,
        config: Config,
        continuation: AsyncStream<(SampledFrame?, Error?)>.Continuation
    ) async throws {
        let asset = AVURLAsset(url: url)
        guard let track = try await asset.loadTracks(withMediaType: .video).first else {
            throw SampleError.noVideoTrack
        }

        let reader = try AVAssetReader(asset: asset)
        let outputSettings: [String: Any] = [
            kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_32BGRA
        ]
        let output = AVAssetReaderTrackOutput(track: track, outputSettings: outputSettings)
        output.alwaysCopiesSampleData = false
        reader.add(output)

        guard reader.startReading() else {
            let desc = reader.error?.localizedDescription ?? "unknown"
            throw SampleError.readerStartFailed(desc)
        }

        let ciContext = CIContext()
        let intervalMs = 1000 / config.fps
        var nextTargetMs = 0
        var finished = false

        while !finished {
            guard let sampleBuffer = output.copyNextSampleBuffer() else {
                finished = true
                break
            }

            let pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer)
            let ptsMs = Int(CMTimeGetSeconds(pts) * 1000)

            // 跳采样：未到目标时间则跳过
            if ptsMs < nextTargetMs { continue }

            guard let pixelBuffer = CMSampleBufferGetImageBuffer(sampleBuffer) else { continue }

            let ciImage = CIImage(cvPixelBuffer: pixelBuffer)
            guard let cgImage = ciContext.createCGImage(ciImage, from: ciImage.extent) else {
                continue
            }

            guard let jpegData = encodeJPEG(cgImage, quality: config.jpegQuality) else {
                throw SampleError.encodeJPEGFailed
            }

            continuation.yield((SampledFrame(tsMs: ptsMs, jpegData: jpegData), nil))
            nextTargetMs += intervalMs
        }

        if reader.status == .reading {
            reader.cancelReading()
        }
        continuation.finish()
    }
}
