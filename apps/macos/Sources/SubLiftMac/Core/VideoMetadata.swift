import AVFoundation
import CoreMedia
import Foundation

/// feat-020：视频元数据（文件名、大小、分辨率、时长、编码）。
struct VideoMetadata: Equatable {
    let fileName: String
    let fileSize: Int64
    let width: Int
    let height: Int
    let durationMs: Int
    let codec: String
}

/// feat-020：视频元数据异步加载器。
@MainActor
final class VideoMetadataLoader: ObservableObject {
    @Published private(set) var metadata: VideoMetadata?

    func load(url: URL) async {
        let fileName = url.lastPathComponent
        let fileSize = getFileSize(url: url)

        if url.pathExtension.lowercased() == "mkv" {
            let durationMs = FfmpegFrameSampler.probeDurationMs(url: url)
            let dimensions = FfmpegFrameSampler.probeDimensions(url: url)
            let codec = FfmpegFrameSampler.probeCodec(url: url)
            metadata = VideoMetadata(
                fileName: fileName,
                fileSize: fileSize,
                width: dimensions?.0 ?? 0,
                height: dimensions?.1 ?? 0,
                durationMs: durationMs,
                codec: codec ?? "unknown"
            )
            return
        }

        await loadWithAVFoundation(url: url, fileName: fileName, fileSize: fileSize)
    }

    func clear() {
        metadata = nil
    }

    // MARK: - Private

    private func loadWithAVFoundation(url: URL, fileName: String, fileSize: Int64) async {
        let asset = AVURLAsset(url: url)

        let durationMs: Int
        if let cmDuration = try? await asset.load(.duration) {
            let secs = CMTimeGetSeconds(cmDuration)
            durationMs = secs.isFinite ? Int(secs * 1000) : 0
        } else {
            durationMs = 0
        }

        guard let track = try? await asset.loadTracks(withMediaType: .video).first else {
            metadata = VideoMetadata(
                fileName: fileName, fileSize: fileSize,
                width: 0, height: 0, durationMs: durationMs, codec: "unknown"
            )
            return
        }

        let size = try? await track.load(.naturalSize)
        let width = Int(size?.width ?? 0)
        let height = Int(size?.height ?? 0)

        let codec = await loadCodec(from: track)

        metadata = VideoMetadata(
            fileName: fileName,
            fileSize: fileSize,
            width: width,
            height: height,
            durationMs: durationMs,
            codec: codec
        )
    }

    private func loadCodec(from track: AVAssetTrack) async -> String {
        let descriptions = try? await track.load(.formatDescriptions)
        guard let desc = descriptions?.first else { return "unknown" }
        let fourCC = CMFormatDescriptionGetMediaSubType(desc)
        return VideoMetadata.formatCodec(fourCC)
    }

    private func getFileSize(url: URL) -> Int64 {
        let attrs = try? FileManager.default.attributesOfItem(atPath: url.path)
        return (attrs?[.size] as? Int64) ?? 0
    }
}

// MARK: - 格式化纯函数（可单测）

extension VideoMetadata {
    /// FourCC 整数转编码名称字符串。
    /// - Parameter fourCC: 如 0x31637661 (avc1) → "H.264"
    static func formatCodec(_ fourCC: FourCharCode) -> String {
        let bytes: [UInt8] = [
            UInt8((fourCC >> 24) & 0xFF),
            UInt8((fourCC >> 16) & 0xFF),
            UInt8((fourCC >> 8) & 0xFF),
            UInt8(fourCC & 0xFF),
        ]
        let raw = String(bytes: bytes, encoding: .ascii) ?? "unknown"

        switch raw {
        case "avc1", "avc3": return "H.264"
        case "hvc1", "hev1": return "HEVC"
        case "mp4v": return "MPEG-4"
        default: return raw
        }
    }

    /// 文件大小格式化。
    /// - Parameter bytes: 字节数
    /// - Returns: 如 "73 MB"、"1.2 GB"
    static func formatFileSize(_ bytes: Int64) -> String {
        let units: [(String, Double)] = [
            ("GB", 1_000_000_000),
            ("MB", 1_000_000),
            ("KB", 1_000),
        ]
        for (suffix, threshold) in units {
            if bytes >= Int64(threshold) {
                return String(format: "%.1f %@", Double(bytes) / threshold, suffix)
            }
        }
        return "\(bytes) B"
    }

    /// 分辨率格式化。
    /// - Parameters: width, height
    /// - Returns: 如 "1920×1080"，0 时返回 "—"
    static func formatResolution(width: Int, height: Int) -> String {
        guard width > 0, height > 0 else { return "—" }
        return "\(width)×\(height)"
    }
}
