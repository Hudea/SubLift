import XCTest
@testable import SubLiftMac

/// 10105：Video Inspector 展示层纯逻辑测试。
///
/// 只展示真实字段：文件名、分辨率、时长、编码、大小、容器、预览兼容性。
final class VideoInspectorPresentationTests: XCTestCase {

    func testContainerNameMapping() {
        XCTAssertEqual(
            VideoInspectorPresentation.containerName(for: URL(fileURLWithPath: "/tmp/a.mp4")),
            "MPEG-4 (MP4)"
        )
        XCTAssertEqual(
            VideoInspectorPresentation.containerName(for: URL(fileURLWithPath: "/tmp/a.MOV")),
            "QuickTime (MOV)"
        )
        XCTAssertEqual(
            VideoInspectorPresentation.containerName(for: URL(fileURLWithPath: "/tmp/a.mkv")),
            "Matroska (MKV)"
        )
        // 未知扩展名：原样大写，不编造容器名。
        XCTAssertEqual(
            VideoInspectorPresentation.containerName(for: URL(fileURLWithPath: "/tmp/a.xyz")),
            "XYZ"
        )
    }

    func testPreviewCompatibilityIsRealNotFake() {
        XCTAssertEqual(VideoInspectorPresentation.previewCompatibility(loadFailed: false), "可正常播放")
        let fallback = VideoInspectorPresentation.previewCompatibility(loadFailed: true)
        XCTAssertTrue(fallback.contains("静态预览"))
        XCTAssertTrue(fallback.contains("ffmpeg"))
    }

    func testRowsContainOnlyRealMetadataFields() {
        let meta = VideoMetadata(
            fileName: "movie.mp4",
            fileSize: 52_428_800, // 50 MB
            width: 1920,
            height: 1080,
            durationMs: 65_321,
            codec: "H.264"
        )
        let url = URL(fileURLWithPath: "/tmp/movie.mp4")
        let rows = VideoInspectorPresentation.rows(metadata: meta, url: url, loadFailed: false)

        // 每个 label 都是真实字段，值来自 metadata 或可推导事实。
        let labels = rows.map(\.label)
        XCTAssertEqual(labels, ["文件名", "分辨率", "时长", "编码", "大小", "容器", "预览"])

        let fileNameRow = rows.first { $0.label == "文件名" }
        XCTAssertEqual(fileNameRow?.value, "movie.mp4")

        let resolutionRow = rows.first { $0.label == "分辨率" }
        XCTAssertEqual(resolutionRow?.value, "1920×1080")

        let codecRow = rows.first { $0.label == "编码" }
        XCTAssertEqual(codecRow?.value, "H.264")

        let sizeRow = rows.first { $0.label == "大小" }
        XCTAssertEqual(sizeRow?.value, "52.4 MB")

        let containerRow = rows.first { $0.label == "容器" }
        XCTAssertEqual(containerRow?.value, "MPEG-4 (MP4)")

        let previewRow = rows.first { $0.label == "预览" }
        XCTAssertEqual(previewRow?.value, "可正常播放")
    }

    func testFallbackPreviewRowReflectsLoadFailed() {
        let meta = VideoMetadata(
            fileName: "movie.mkv",
            fileSize: 10_000_000,
            width: 640,
            height: 360,
            durationMs: 5000,
            codec: "unknown"
        )
        let url = URL(fileURLWithPath: "/tmp/movie.mkv")
        let rows = VideoInspectorPresentation.rows(metadata: meta, url: url, loadFailed: true)

        let previewRow = rows.first { $0.label == "预览" }
        XCTAssertTrue(previewRow?.value.contains("静态预览") == true)
        let containerRow = rows.first { $0.label == "容器" }
        XCTAssertEqual(containerRow?.value, "Matroska (MKV)")
    }

    func testZeroMetadataShowsDashNotFakeValues() {
        let meta = VideoMetadata(
            fileName: "empty.mp4",
            fileSize: 0,
            width: 0,
            height: 0,
            durationMs: 0,
            codec: "unknown"
        )
        let rows = VideoInspectorPresentation.rows(
            metadata: meta,
            url: URL(fileURLWithPath: "/tmp/empty.mp4"),
            loadFailed: false
        )
        let resolutionRow = rows.first { $0.label == "分辨率" }
        XCTAssertEqual(resolutionRow?.value, "—")
    }
}

/// 10105：Inspector 模式分发与关闭保持。
@MainActor
final class WorkspaceInspectorModeTests: XCTestCase {

    private func makeModel() -> WorkspaceModel {
        WorkspaceModel(metadataRequest: { url in
            try? await Task.sleep(nanoseconds: 60_000_000_000)
            return VideoMetadata(fileName: url.lastPathComponent, fileSize: 0, width: 0, height: 0, durationMs: 0, codec: "unknown")
        })
    }

    func testOpenVideoDefaultsInspectorToVideoMode() {
        let model = makeModel()
        model.openVideo(url: URL(fileURLWithPath: "/tmp/v.mp4"))
        XCTAssertEqual(model.inspectorMode, .video)
    }

    func testUserClosedInspectorIsNotForcedReopenedByTransitions() {
        let model = makeModel()
        model.openVideo(url: URL(fileURLWithPath: "/tmp/v.mp4"))
        let token = model.sessionToken
        model.handleMetadataLoaded(
            VideoMetadata(fileName: "v.mp4", fileSize: 100, width: 1920, height: 1080, durationMs: 1000, codec: "h264"),
            sessionToken: token
        )

        model.setInspectorPresented(true)
        model.toggleInspector() // 用户关闭
        XCTAssertFalse(model.isInspectorPresented)

        // 状态转换（region、extraction、review）不得强制重新打开。
        model.enterRegionEditing()
        model.exitRegionEditing()
        model.startExtraction()
        let jobToken = model.jobToken!
        model.handleExtractionProgress(progress: 0.5, frameCount: 5, totalFrames: 10, jobToken: jobToken)
        model.handleExtractionFinalEntries(
            [SubtitleEntryData(startMs: 0, endMs: 100, text: "Hi", confidence: 1.0)],
            jobToken: jobToken
        )

        XCTAssertFalse(model.isInspectorPresented)
        XCTAssertEqual(model.state, .review)
    }

    func testInspectorWidthConstantInDesignRange() {
        XCTAssertGreaterThanOrEqual(WorkspaceLayout.inspectorReservedWidth, 280)
        XCTAssertLessThanOrEqual(WorkspaceLayout.inspectorReservedWidth, 360)
    }
}
