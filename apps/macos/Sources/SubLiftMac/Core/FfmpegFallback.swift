import AppKit
import Foundation

/// feat-019：系统 ffmpeg 检测 + mkv 抽帧兜底。
///
/// AVFoundation 不支持 mkv 容器（feat-012 spike 实测 -11828），
/// 用系统 ffmpeg 抽 MJPEG 流到 stdout，按 SOI/EOI marker 切帧。

// MARK: - FfmpegDetector

enum FfmpegDetector {

    /// 缓存的 ffmpeg 路径（nil 表示未检测或不存在）。
    private static var _cachedPath: String?
    private static var _detected = false

    /// 检测系统 ffmpeg 是否可用。
    /// - Returns: ffmpeg 可执行文件路径；不存在返回 nil。
    static func detect() -> String? {
        if _detected { return _cachedPath }
        _detected = true

        let process = Process()
        process.executableURL = URL(fileURLWithPath: "/usr/bin/env")
        process.arguments = ["which", "ffmpeg"]

        let pipe = Pipe()
        process.standardOutput = pipe
        process.standardError = Pipe()

        do {
            try process.run()
            process.waitUntilExit()
        } catch {
            return nil
        }

        guard process.terminationStatus == 0 else { return nil }

        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        let path = String(data: data, encoding: .utf8)?
            .trimmingCharacters(in: .whitespacesAndNewlines)
        _cachedPath = (path?.isEmpty == false) ? path : nil
        return _cachedPath
    }

    /// 重置缓存（测试用）。
    static func resetCache() {
        _cachedPath = nil
        _detected = false
    }
}

// MARK: - MjpegParser

/// MJPEG 字节流解析器：按 JPEG SOI/EOI marker 切帧。
///
/// JPEG 帧结构：FF D8（SOI）... 帧体 ... FF D9（EOI）。
/// SOI/EOI 只在帧边界出现，直接搜索即可。
enum MjpegParser {

    /// 从缓冲区解析下一帧完整 JPEG。
    ///
    /// - Parameter buffer: 输入字节缓冲区。解析成功后会移除已消费的字节。
    /// - Returns: 完整的 JPEG Data（含 SOI 和 EOI）；缓冲区中无完整帧时返回 nil。
    static func parseNextJPEG(from buffer: inout Data) -> Data? {
        // 1. 找 SOI（FF D8）
        guard let soiRange = buffer.range(of: Data([0xFF, 0xD8])) else {
            // 没有 SOI，丢弃缓冲区前面的无效字节（保留最后 1 字节，可能 FF 是 SOI 开头）
            if buffer.count > 1 {
                buffer = Data(buffer.suffix(1))
            }
            return nil
        }

        // 2. 从 SOI 开始找 EOI（FF D9）
        let searchStart = soiRange.upperBound
        guard searchStart < buffer.endIndex else { return nil }

        let eoiMarker = Data([0xFF, 0xD9])
        guard let eoiRange = buffer.range(of: eoiMarker, in: searchStart..<buffer.endIndex) else {
            // 找到 SOI 但还没 EOI，等更多数据。丢弃 SOI 之前的无效字节。
            if soiRange.lowerBound > buffer.startIndex {
                buffer = Data(buffer.suffix(from: soiRange.lowerBound))
            }
            return nil
        }

        // 3. 提取完整 JPEG（SOI ... EOI，含 EOI 两字节）
        let frameEnd = eoiRange.upperBound
        let jpegData = Data(buffer[soiRange.lowerBound..<frameEnd])

        // 4. 从 buffer 移除已消费字节（用 Data() 重建避免索引偏移问题）
        if frameEnd == buffer.endIndex {
            buffer = Data()
        } else {
            buffer = Data(buffer[frameEnd..<buffer.endIndex])
        }

        return jpegData
    }
}

// MARK: - FfmpegFrameSampler

/// 用 ffmpeg 抽帧，输出与 FrameSampler.SampledFrame 等价的帧流。
enum FfmpegFrameSampler {

    enum SampleError: Error, Equatable {
        case ffmpegNotFound
        case ffmpegLaunchFailed(String)
        case noFrames
    }

    /// 用 ffmpeg 抽指定时间点的帧作为预览图（mkv 等 AVPlayer 不支持的格式）。
    /// - Parameters:
    ///   - url: 视频文件 URL
    ///   - seconds: 目标时间点（秒）
    /// - Returns: 预览 NSImage；失败返回 nil
    static func captureFrameAt(url: URL, seconds: Double) -> NSImage? {
        guard let ffmpegPath = FfmpegDetector.detect() else { return nil }

        let process = Process()
        process.executableURL = URL(fileURLWithPath: ffmpegPath)
        // -ss 在 -i 前是 fast seek（关键帧定位，快但不精确）
        process.arguments = [
            "-ss", String(format: "%.2f", seconds),
            "-i", url.path,
            "-frames:v", "1",
            "-f", "image2pipe",
            "-vcodec", "mjpeg",
            "-q:v", "2",
            "-",
        ]

        let stdoutPipe = Pipe()
        process.standardOutput = stdoutPipe
        process.standardError = Pipe()

        do {
            try process.run()
        } catch {
            return nil
        }

        let data = stdoutPipe.fileHandleForReading.readDataToEndOfFile()
        process.waitUntilExit()

        guard process.terminationStatus == 0, !data.isEmpty else { return nil }
        return NSImage(data: data)
    }

    /// 用 ffprobe 获取视频时长（毫秒）。
    /// - Parameter url: 视频文件 URL
    /// - Returns: 时长（毫秒）；失败返回 0
    static func probeDurationMs(url: URL) -> Int {
        guard let ffmpegPath = FfmpegDetector.detect() else { return 0 }

        let ffprobePath = (ffmpegPath as NSString)
            .deletingLastPathComponent + "/ffprobe"

        let process = Process()
        process.executableURL = URL(fileURLWithPath: ffprobePath)
        process.arguments = [
            "-v", "error",
            "-show_entries", "format=duration",
            "-of", "csv=p=0",
            url.path,
        ]

        let pipe = Pipe()
        process.standardOutput = pipe
        process.standardError = Pipe()

        do {
            try process.run()
            process.waitUntilExit()
        } catch {
            return 0
        }

        guard process.terminationStatus == 0 else { return 0 }

        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        guard let output = String(data: data, encoding: .utf8)?
            .trimmingCharacters(in: .whitespacesAndNewlines),
              let secs = Double(output) else { return 0 }

        return Int(secs * 1000)
    }

    /// 用 ffprobe 获取视频分辨率。
    /// - Parameter url: 视频文件 URL
    /// - Returns: (width, height)；失败返回 nil
    static func probeDimensions(url: URL) -> (Int, Int)? {
        guard let ffmpegPath = FfmpegDetector.detect() else { return nil }
        let ffprobePath = (ffmpegPath as NSString).deletingLastPathComponent + "/ffprobe"

        let process = Process()
        process.executableURL = URL(fileURLWithPath: ffprobePath)
        process.arguments = [
            "-v", "error",
            "-select_streams", "v:0",
            "-show_entries", "stream=width,height",
            "-of", "csv=s=x:p=0",
            url.path,
        ]

        let pipe = Pipe()
        process.standardOutput = pipe
        process.standardError = Pipe()

        do {
            try process.run()
            process.waitUntilExit()
        } catch {
            return nil
        }

        guard process.terminationStatus == 0 else { return nil }

        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        guard let output = String(data: data, encoding: .utf8)?
            .trimmingCharacters(in: .whitespacesAndNewlines) else { return nil }

        let parts = output.split(separator: "x")
        guard parts.count == 2,
              let w = Int(parts[0]),
              let h = Int(parts[1]) else { return nil }
        return (w, h)
    }

    /// 用 ffprobe 获取视频编码名称。
    /// - Parameter url: 视频文件 URL
    /// - Returns: 编码名称（如 "h264"、"hevc"）；失败返回 nil
    static func probeCodec(url: URL) -> String? {
        guard let ffmpegPath = FfmpegDetector.detect() else { return nil }
        let ffprobePath = (ffmpegPath as NSString).deletingLastPathComponent + "/ffprobe"

        let process = Process()
        process.executableURL = URL(fileURLWithPath: ffprobePath)
        process.arguments = [
            "-v", "error",
            "-select_streams", "v:0",
            "-show_entries", "stream=codec_name",
            "-of", "csv=p=0",
            url.path,
        ]

        let pipe = Pipe()
        process.standardOutput = pipe
        process.standardError = Pipe()

        do {
            try process.run()
            process.waitUntilExit()
        } catch {
            return nil
        }

        guard process.terminationStatus == 0 else { return nil }

        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        guard let output = String(data: data, encoding: .utf8)?
            .trimmingCharacters(in: .whitespacesAndNewlines) else { return nil }

        return output.isEmpty ? nil : output
    }

    /// spawn ffmpeg 抽帧，返回 AsyncStream。
    ///
    /// 命令：`ffmpeg -i <input> -vf fps=<fps> -f image2pipe -vcodec mjpeg -`
    /// - Parameters:
    ///   - url: 视频文件 URL
    ///   - config: 抽帧参数（用 fps 和 jpegQuality）
    static func sample(
        url: URL,
        config: FrameSampler.Config
    ) -> AsyncStream<(FrameSampler.SampledFrame?, Error?)> {
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

    /// 用 ffprobe 估算总帧数（mkv 无法用 AVURLAsset.duration）。
    static func estimateFrameCount(url: URL, fps: Int) async -> Int {
        guard let ffmpegPath = FfmpegDetector.detect() else { return 0 }

        // ffprobe 通常和 ffmpeg 同目录
        let ffprobePath = (ffmpegPath as NSString)
            .deletingLastPathComponent + "/ffprobe"

        let process = Process()
        process.executableURL = URL(fileURLWithPath: ffprobePath)
        process.arguments = [
            "-v", "error",
            "-show_entries", "format=duration",
            "-of", "csv=p=0",
            url.path,
        ]

        let pipe = Pipe()
        process.standardOutput = pipe
        process.standardError = Pipe()

        do {
            try process.run()
            process.waitUntilExit()
        } catch {
            return 0
        }

        guard process.terminationStatus == 0 else { return 0 }

        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        guard let output = String(data: data, encoding: .utf8)?
            .trimmingCharacters(in: .whitespacesAndNewlines),
              let secs = Double(output) else { return 0 }

        return Int(secs * Double(fps))
    }

    // MARK: - Private

    private static func sampleImpl(
        url: URL,
        config: FrameSampler.Config,
        continuation: AsyncStream<(FrameSampler.SampledFrame?, Error?)>.Continuation
    ) async throws {
        guard let ffmpegPath = FfmpegDetector.detect() else {
            throw SampleError.ffmpegNotFound
        }

        let process = Process()
        process.executableURL = URL(fileURLWithPath: ffmpegPath)
        process.arguments = [
            "-i", url.path,
            "-vf", "fps=\(config.fps)",
            "-f", "image2pipe",
            "-vcodec", "mjpeg",
            "-q:v", "2",  // JPEG 质量（2≈high，对应 q≈0.85）
            "-",         // 输出到 stdout
        ]

        let stdoutPipe = Pipe()
        process.standardOutput = stdoutPipe
        process.standardError = Pipe()  // 忽略 stderr

        do {
            try process.run()
        } catch {
            throw SampleError.ffmpegLaunchFailed(error.localizedDescription)
        }

        let intervalMs = 1000 / config.fps
        var frameIndex = 0
        var buffer = Data()
        let handle = stdoutPipe.fileHandleForReading

        // 读 stdout 直到 ffmpeg 结束
        while process.isRunning {
            let chunk = handle.availableData
            if chunk.isEmpty { break }
            buffer.append(chunk)

            // 尝试从缓冲区解析所有完整帧
            while let jpegData = MjpegParser.parseNextJPEG(from: &buffer) {
                let tsMs = frameIndex * intervalMs
                continuation.yield((FrameSampler.SampledFrame(tsMs: tsMs, jpegData: jpegData), nil))
                frameIndex += 1
            }
        }

        // ffmpeg 结束后，读剩余数据并解析
        let remaining = handle.readDataToEndOfFile()
        buffer.append(remaining)
        while let jpegData = MjpegParser.parseNextJPEG(from: &buffer) {
            let tsMs = frameIndex * intervalMs
            continuation.yield((FrameSampler.SampledFrame(tsMs: tsMs, jpegData: jpegData), nil))
            frameIndex += 1
        }

        process.waitUntilExit()

        if frameIndex == 0 {
            throw SampleError.noFrames
        }
    }
}
