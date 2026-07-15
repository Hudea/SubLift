import Foundation

/// 将已处理的视频时长与实际处理耗时换算为相对实时的处理速率。
///
/// 例如 5 fps 下已处理 60 个采样帧，耗时 3 秒，等价于处理了 12 秒视频，
/// 因而显示为 `4.0× 实时`。这是处理吞吐量，并非原视频的播放倍速。
enum ProcessingRate {

    /// 避免任务刚开始时因计时粒度导致不稳定的超大倍速。
    private static let minimumElapsedSeconds: TimeInterval = 0.25

    static func realTimeMultiplier(
        processedFrames: Int,
        sampleFps: Int,
        elapsedSeconds: TimeInterval
    ) -> Double? {
        guard processedFrames > 0,
              sampleFps > 0,
              elapsedSeconds >= minimumElapsedSeconds,
              elapsedSeconds.isFinite
        else {
            return nil
        }

        let processedVideoSeconds = Double(processedFrames) / Double(sampleFps)
        let multiplier = processedVideoSeconds / elapsedSeconds
        return multiplier.isFinite && multiplier >= 0 ? multiplier : nil
    }

    static func displayText(for multiplier: Double?) -> String? {
        guard let multiplier, multiplier.isFinite, multiplier >= 0 else { return nil }
        return String(format: "%.1f× 实时", multiplier)
    }
}
