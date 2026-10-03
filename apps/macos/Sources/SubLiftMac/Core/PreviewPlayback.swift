import AVFoundation
import Foundation

/// 预览解码器选择：以 AVFoundation 能力为准，不看容器扩展名。
///
/// MP4 只是容器。视频轨 `isPlayable == false`（例如部分 HEVC）时，
/// AVPlayer 会黑屏，ROI 也无法从播放器取帧；此时与 mkv 一样走 ffmpeg。
enum PreviewPlayback {

    /// 是否应使用 ffmpeg 静态预览 / 抽帧。
    ///
    /// - `itemStatus == .failed`：播放器明确失败（mkv 等）。
    /// - `videoTrackPlayable == false`：资源可读但视频轨不可播（HEVC `hev1` 等）。
    /// - 能力未知时不抢先 fallback，等 item 状态。
    static func shouldUseFfmpegFallback(
        videoTrackPlayable: Bool?,
        itemStatus: AVPlayerItem.Status
    ) -> Bool {
        if itemStatus == .failed { return true }
        if videoTrackPlayable == false { return true }
        return false
    }

    /// 查询视频轨是否可被 AVPlayer / AVAssetReader 播放。
    /// - Returns: `true`/`false`；资源无法探测时 `nil`。
    static func videoTrackIsPlayable(url: URL) async -> Bool? {
        let asset = AVURLAsset(url: url)
        do {
            let tracks = try await asset.loadTracks(withMediaType: .video)
            guard let track = tracks.first else { return false }
            return try await track.load(.isPlayable)
        } catch {
            return nil
        }
    }
}
