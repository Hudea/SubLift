import AVFoundation
import Testing
@testable import SubLiftMac

struct PreviewPlaybackTests {

    @Test
    func failedItemAlwaysFallsBackRegardlessOfPlayableFlag() {
        #expect(
            PreviewPlayback.shouldUseFfmpegFallback(
                videoTrackPlayable: true,
                itemStatus: .failed
            )
        )
        #expect(
            PreviewPlayback.shouldUseFfmpegFallback(
                videoTrackPlayable: nil,
                itemStatus: .failed
            )
        )
    }

    @Test
    func unplayableVideoTrackFallsBackEvenWhenItemIsReady() {
        // 实测 HEVC hev1 MP4：duration/tracks 可读，但 video isPlayable=false，
        // item 仍可能 readyToPlay（音轨可播），不能因此走 AVPlayer。
        #expect(
            PreviewPlayback.shouldUseFfmpegFallback(
                videoTrackPlayable: false,
                itemStatus: .readyToPlay
            )
        )
        #expect(
            PreviewPlayback.shouldUseFfmpegFallback(
                videoTrackPlayable: false,
                itemStatus: .unknown
            )
        )
    }

    @Test
    func playableTrackWithReadyItemUsesAVPlayer() {
        #expect(
            !PreviewPlayback.shouldUseFfmpegFallback(
                videoTrackPlayable: true,
                itemStatus: .readyToPlay
            )
        )
    }

    @Test
    func unknownCapabilityDoesNotPreemptFallback() {
        #expect(
            !PreviewPlayback.shouldUseFfmpegFallback(
                videoTrackPlayable: nil,
                itemStatus: .unknown
            )
        )
        #expect(
            !PreviewPlayback.shouldUseFfmpegFallback(
                videoTrackPlayable: nil,
                itemStatus: .readyToPlay
            )
        )
        #expect(
            !PreviewPlayback.shouldUseFfmpegFallback(
                videoTrackPlayable: true,
                itemStatus: .unknown
            )
        )
    }

    @Test
    func decisionDoesNotInspectFileExtension() {
        // 政策函数不接收 URL：扩展名不能成为分支。
        let mp4Unplayable = PreviewPlayback.shouldUseFfmpegFallback(
            videoTrackPlayable: false,
            itemStatus: .readyToPlay
        )
        let mkvFailed = PreviewPlayback.shouldUseFfmpegFallback(
            videoTrackPlayable: nil,
            itemStatus: .failed
        )
        #expect(mp4Unplayable)
        #expect(mkvFailed)
    }
}
