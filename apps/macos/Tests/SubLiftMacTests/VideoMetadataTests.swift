import Foundation
import Testing
@testable import SubLiftMac

/// feat-020：VideoMetadata 纯函数单测（格式化）。
struct VideoMetadataTests {

    @Test @MainActor
    func fetchDoesNotPublishBeforeCoordinatorValidation() async {
        let loader = VideoMetadataLoader()
        let url = URL(fileURLWithPath: "/tmp/nonexistent-metadata-test.mp4")

        let fetched = await loader.fetch(url: url)

        #expect(fetched.fileName == url.lastPathComponent)
        #expect(loader.metadata == nil)

        loader.publish(fetched)
        #expect(loader.metadata == fetched)
    }

    // MARK: - formatCodec

    @Test
    func formatCodecAVC1() {
        // "avc1" = 0x61 0x76 0x63 0x31
        let result = VideoMetadata.formatCodec(0x61766331)
        #expect(result == "H.264")
    }

    @Test
    func formatCodecAVC3() {
        // "avc3" = 0x61 0x76 0x63 0x33
        let result = VideoMetadata.formatCodec(0x61766333)
        #expect(result == "H.264")
    }

    @Test
    func formatCodecHVC1() {
        // "hvc1" = 0x68 0x76 0x63 0x31
        let result = VideoMetadata.formatCodec(0x68766331)
        #expect(result == "HEVC")
    }

    @Test
    func formatCodecHEV1() {
        // "hev1" = 0x68 0x65 0x76 0x31
        let result = VideoMetadata.formatCodec(0x68657631)
        #expect(result == "HEVC")
    }

    @Test
    func formatCodecUnknown() {
        // 非 ASCII 字节
        let result = VideoMetadata.formatCodec(0xFFFFFFFF)
        #expect(result == "unknown")
    }

    // MARK: - formatFileSize

    @Test
    func formatFileSizeBytes() {
        #expect(VideoMetadata.formatFileSize(500) == "500 B")
    }

    @Test
    func formatFileSizeKB() {
        #expect(VideoMetadata.formatFileSize(1500) == "1.5 KB")
    }

    @Test
    func formatFileSizeMB() {
        #expect(VideoMetadata.formatFileSize(73_000_000) == "73.0 MB")
    }

    @Test
    func formatFileSizeGB() {
        #expect(VideoMetadata.formatFileSize(1_200_000_000) == "1.2 GB")
    }

    @Test
    func formatFileSizeZero() {
        #expect(VideoMetadata.formatFileSize(0) == "0 B")
    }

    // MARK: - formatResolution

    @Test
    func formatResolution1080p() {
        #expect(VideoMetadata.formatResolution(width: 1920, height: 1080) == "1920×1080")
    }

    @Test
    func formatResolution720p() {
        #expect(VideoMetadata.formatResolution(width: 1280, height: 720) == "1280×720")
    }

    @Test
    func formatResolutionZeroReturnsDash() {
        #expect(VideoMetadata.formatResolution(width: 0, height: 0) == "—")
    }

    @Test
    func formatResolutionPartialZeroReturnsDash() {
        #expect(VideoMetadata.formatResolution(width: 1920, height: 0) == "—")
    }
}
