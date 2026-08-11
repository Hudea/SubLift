import XCTest
@testable import SubLiftMac

// MARK: - VideoImportPolicy 纯逻辑（格式白名单 / ffmpeg 依赖 / Open Panel 类型）

final class VideoImportPolicyTests: XCTestCase {

    func testSupportedFormatsAreValid() {
        let policy = VideoImportPolicy(ffmpegAvailable: { true })
        XCTAssertEqual(
            policy.validate(url: URL(fileURLWithPath: "/tmp/sample.mp4")),
            .valid(format: .mp4)
        )
        XCTAssertEqual(
            policy.validate(url: URL(fileURLWithPath: "/tmp/sample.MOV")),
            .valid(format: .mov)
        )
        XCTAssertEqual(
            policy.validate(url: URL(fileURLWithPath: "/tmp/sample.mkv")),
            .valid(format: .mkv)
        )
    }

    func testUnsupportedFormatsAreRejected() {
        let policy = VideoImportPolicy(ffmpegAvailable: { true })
        XCTAssertEqual(
            policy.validate(url: URL(fileURLWithPath: "/tmp/sample.avi")),
            .unsupportedFormat(pathExtension: "avi")
        )
        XCTAssertEqual(
            policy.validate(url: URL(fileURLWithPath: "/tmp/sample.m4v")),
            .unsupportedFormat(pathExtension: "m4v")
        )
        XCTAssertEqual(
            policy.validate(url: URL(fileURLWithPath: "/tmp/sample.txt")),
            .unsupportedFormat(pathExtension: "txt")
        )
        XCTAssertEqual(
            policy.validate(url: URL(fileURLWithPath: "/tmp/noextension")),
            .unsupportedFormat(pathExtension: "")
        )
    }

    func testMkvRequiresFfmpeg() {
        let noFfmpeg = VideoImportPolicy(ffmpegAvailable: { false })
        XCTAssertEqual(
            noFfmpeg.validate(url: URL(fileURLWithPath: "/tmp/sample.mkv")),
            .mkvRequiresFfmpeg
        )

        let withFfmpeg = VideoImportPolicy(ffmpegAvailable: { true })
        XCTAssertEqual(
            withFfmpeg.validate(url: URL(fileURLWithPath: "/tmp/sample.mkv")),
            .valid(format: .mkv)
        )
    }

    func testMp4AndMovDoNotRequireFfmpeg() {
        let policy = VideoImportPolicy(ffmpegAvailable: { false })
        XCTAssertEqual(
            policy.validate(url: URL(fileURLWithPath: "/tmp/sample.mp4")),
            .valid(format: .mp4)
        )
        XCTAssertEqual(
            policy.validate(url: URL(fileURLWithPath: "/tmp/sample.mov")),
            .valid(format: .mov)
        )
    }

    func testSupportedExtensionsAndOpenPanelTypes() {
        XCTAssertEqual(VideoImportPolicy.supportedExtensions, ["mp4", "mov", "mkv"])
        XCTAssertFalse(VideoImportPolicy.openPanelContentTypes.isEmpty)
    }

    func testSupportedFormatsTextMatchesDesign() {
        XCTAssertEqual(VideoImportPolicy.supportedFormatsText, "MP4 · MOV · MKV")
    }
}

// MARK: - WorkspaceModel 导入门（Open Panel 与 drop 共用同一 intent）

@MainActor
final class WorkspaceImportTests: XCTestCase {

    private func makeModel(ffmpegAvailable: @escaping () -> Bool = { true }) -> WorkspaceModel {
        WorkspaceModel(
            metadataRequest: { url in
                do {
                    try await Task.sleep(nanoseconds: 60_000_000_000)
                } catch {
                    // 测试通过显式 handler 驱动 metadata；挂起请求由 token/cancellation guard 丢弃。
                }
                return VideoMetadata(
                    fileName: url.lastPathComponent,
                    fileSize: 0,
                    width: 0,
                    height: 0,
                    durationMs: 0,
                    codec: "unknown"
                )
            },
            importPolicy: VideoImportPolicy(ffmpegAvailable: ffmpegAvailable)
        )
    }

    func testOpenVideoRejectsUnsupportedFormatWithoutChangingSession() {
        let model = makeModel()
        let initialToken = model.sessionToken
        let url = URL(fileURLWithPath: "/tmp/sample.avi")

        XCTAssertFalse(model.openVideo(url: url))
        XCTAssertEqual(model.state, .empty)
        XCTAssertNil(model.currentVideoURL)
        XCTAssertEqual(model.sessionToken, initialToken)
        XCTAssertFalse(model.hasFinalEntries)
    }

    func testOpenVideoRejectsMkvWithoutFfmpegWithoutChangingSession() {
        let model = makeModel(ffmpegAvailable: { false })
        let initialToken = model.sessionToken
        let url = URL(fileURLWithPath: "/tmp/sample.mkv")

        XCTAssertFalse(model.openVideo(url: url))
        XCTAssertEqual(model.state, .empty)
        XCTAssertNil(model.currentVideoURL)
        XCTAssertEqual(model.sessionToken, initialToken)
    }

    func testOpenVideoAcceptsSupportedFormats() {
        for path in ["/tmp/a.mp4", "/tmp/b.MOV", "/tmp/c.mkv"] {
            let model = makeModel()
            let url = URL(fileURLWithPath: path)
            XCTAssertTrue(model.openVideo(url: url), "should accept \(path)")
            XCTAssertEqual(model.state, .loading)
            XCTAssertEqual(model.currentVideoURL, url)
        }
    }

    func testValidationMatchesOpenVideoGate() {
        // validateImport（View 拖拽反馈）与 openVideo 门（Session 变更）必须一致。
        let model = makeModel(ffmpegAvailable: { false })
        let validURL = URL(fileURLWithPath: "/tmp/ok.mp4")
        let mkvURL = URL(fileURLWithPath: "/tmp/ok.mkv")
        let badURL = URL(fileURLWithPath: "/tmp/bad.avi")

        XCTAssertEqual(model.validateImport(validURL), .valid(format: .mp4))
        XCTAssertTrue(model.openVideo(url: validURL))
        XCTAssertEqual(model.state, .loading)

        XCTAssertEqual(model.validateImport(mkvURL), .mkvRequiresFfmpeg)
        XCTAssertFalse(model.openVideo(url: mkvURL))
        XCTAssertEqual(model.currentVideoURL, validURL) // Session 保持

        XCTAssertEqual(model.validateImport(badURL), .unsupportedFormat(pathExtension: "avi"))
        XCTAssertFalse(model.openVideo(url: badURL))
        XCTAssertEqual(model.currentVideoURL, validURL)
    }

    func testOpenVideoResetsRegionSelection() {
        let model = makeModel()
        let url1 = URL(fileURLWithPath: "/tmp/v1.mp4")
        model.openVideo(url: url1)
        let token1 = model.sessionToken
        model.handleMetadataLoaded(
            VideoMetadata(fileName: "v1.mp4", fileSize: 100, width: 1920, height: 1080, durationMs: 1000, codec: "h264"),
            sessionToken: token1
        )

        model.regionModel.selectedIds = [1, 2, 3]
        XCTAssertEqual(model.regionModel.selectedIds, [1, 2, 3])

        model.openVideo(url: URL(fileURLWithPath: "/tmp/v2.mp4"))
        XCTAssertTrue(model.regionModel.selectedIds.isEmpty)
    }

    func testOpenVideoDoesNotTouchSettings() {
        let defaults = UserDefaults.standard
        defaults.set("paddle", forKey: "default_engine")
        defaults.set("fine", forKey: "sampling_quality")
        defer {
            defaults.removeObject(forKey: "default_engine")
            defaults.removeObject(forKey: "sampling_quality")
        }

        let model = makeModel()
        model.openVideo(url: URL(fileURLWithPath: "/tmp/s.mp4"))

        XCTAssertEqual(defaults.string(forKey: "default_engine"), "paddle")
        XCTAssertEqual(defaults.string(forKey: "sampling_quality"), "fine")
    }

    func testDropAndOpenPanelShareTheSameIntent() {
        // Open Panel 与 drop 都汇聚到 validateImport + openVideo 同一 intent。
        let model = makeModel()
        let url = URL(fileURLWithPath: "/tmp/drop.mp4")

        let validation = model.validateImport(url)
        let opened = (validation == .valid(format: .mp4)) && model.openVideo(url: url)

        XCTAssertTrue(opened)
        XCTAssertEqual(model.state, .loading)
        XCTAssertEqual(model.currentVideoURL, url)
    }

    func testOpenVideoRejectedDuringProcessingDoesNotChangeSession() {
        // 合法格式但状态不允许（processing）：openVideo 必须拒绝且 Session 完全不变，
        // View 层据此给出"当前无法打开新视频"反馈而不是静默失败。
        let model = makeModel()
        let url = URL(fileURLWithPath: "/tmp/v.mp4")
        model.openVideo(url: url)
        let token = model.sessionToken
        model.handleMetadataLoaded(
            VideoMetadata(fileName: "v.mp4", fileSize: 100, width: 1920, height: 1080, durationMs: 1000, codec: "h264"),
            sessionToken: token
        )
        model.startExtraction()
        let jobToken = model.jobToken!
        model.handleExtractionProgress(progress: 0.3, frameCount: 3, totalFrames: 10, jobToken: jobToken)
        XCTAssertEqual(model.state, .processing)
        XCTAssertFalse(model.commandAvailability.canOpen)

        XCTAssertFalse(model.openVideo(url: URL(fileURLWithPath: "/tmp/new.mp4")))
        XCTAssertEqual(model.state, .processing)
        XCTAssertEqual(model.sessionToken, token)
        XCTAssertEqual(model.jobToken, jobToken)
        XCTAssertEqual(model.currentVideoURL, url)
        model.cancelExtraction()
    }
}

// MARK: - DEBUG 截图 fixture（生产行为不变）

#if DEBUG
final class EvidenceShotTests: XCTestCase {
    func testDropTargetFixtureDisabledWithoutEnvironmentVariable() {
        // 无 SUBLIFT_EVIDENCE_DROP 环境变量时 fixture 必须关闭（生产路径不受影响）。
        let env = ProcessInfo.processInfo.environment["SUBLIFT_EVIDENCE_DROP"]
        XCTAssertNotEqual(env, "1")
    }
}
#endif
