import XCTest
@testable import SubLiftMac

@MainActor
final class WorkspaceModelTests: XCTestCase {

    private func makeModel() -> WorkspaceModel {
        WorkspaceModel(metadataRequest: { url in
            do {
                try await Task.sleep(nanoseconds: 60_000_000_000)
            } catch {
                // 测试通过显式 handler 驱动 metadata；取消后返回值会被 token/cancellation guard 丢弃。
            }
            return VideoMetadata(
                fileName: url.lastPathComponent,
                fileSize: 0,
                width: 0,
                height: 0,
                durationMs: 0,
                codec: "unknown"
            )
        })
    }

    func testInitialStateIsEmpty() {
        let model = makeModel()
        XCTAssertEqual(model.state, .empty)
        XCTAssertNil(model.currentVideoURL)
        XCTAssertEqual(model.inspectorMode, .video)
        XCTAssertFalse(model.isInspectorPresented)
        XCTAssertNil(model.errorMessage)
        XCTAssertNil(model.jobToken)
        XCTAssertFalse(model.hasFinalEntries)
        XCTAssertEqual(model.transcriptAccessMode, .readOnly)
        XCTAssertEqual(model.commandAvailability, WorkspaceCommandAvailability.derive(state: .empty, hasFinalEntries: false))
    }

    func testEmptyToLoadingToReady() {
        let model = makeModel()
        let url = URL(fileURLWithPath: "/tmp/test.mp4")
        model.openVideo(url: url)

        XCTAssertEqual(model.state, .loading)
        XCTAssertEqual(model.currentVideoURL, url)
        let token = model.sessionToken

        let meta = VideoMetadata(
            fileName: "test.mp4",
            fileSize: 1024000,
            width: 1920,
            height: 1080,
            durationMs: 60000,
            codec: "h264"
        )
        model.handleMetadataLoaded(meta, sessionToken: token)

        XCTAssertEqual(model.state, .ready)
        XCTAssertNil(model.errorMessage)
    }

    func testReadyToRegionEditingToReady() {
        let model = makeModel()
        let url = URL(fileURLWithPath: "/tmp/test.mp4")
        model.openVideo(url: url)
        let token = model.sessionToken
        let meta = VideoMetadata(fileName: "test.mp4", fileSize: 100, width: 1920, height: 1080, durationMs: 1000, codec: "h264")
        model.handleMetadataLoaded(meta, sessionToken: token)

        model.enterRegionEditing()
        XCTAssertEqual(model.state, .regionEditing)
        XCTAssertEqual(model.inspectorMode, .region)
        XCTAssertTrue(model.commandAvailability.isRegionActive)

        model.exitRegionEditing()
        XCTAssertEqual(model.state, .ready)
        XCTAssertEqual(model.inspectorMode, .video)
        XCTAssertFalse(model.commandAvailability.isRegionActive)
    }

    func testExtractionLifecycleToReview() {
        let model = makeModel()
        let url = URL(fileURLWithPath: "/tmp/test.mp4")
        model.openVideo(url: url)
        let sessionToken = model.sessionToken
        model.handleMetadataLoaded(VideoMetadata(fileName: "test.mp4", fileSize: 100, width: 1920, height: 1080, durationMs: 1000, codec: "h264"), sessionToken: sessionToken)

        model.startExtraction()
        XCTAssertEqual(model.state, .starting)
        XCTAssertEqual(model.inspectorMode, .extraction)
        XCTAssertFalse(model.hasFinalEntries)
        guard let jobToken = model.jobToken else {
            XCTFail("jobToken should not be nil during extraction")
            return
        }

        model.handleExtractionProgress(progress: 0.5, frameCount: 5, totalFrames: 10, jobToken: jobToken)
        XCTAssertEqual(model.state, .processing)

        model.handleExtractionFinalizing(jobToken: jobToken)
        XCTAssertEqual(model.state, .finalizing)

        let entriesData = [SubtitleEntryData(startMs: 0, endMs: 1000, text: "Hello World", confidence: 0.9)]
        model.handleExtractionFinalEntries(entriesData, jobToken: jobToken)

        XCTAssertEqual(model.state, .review)
        XCTAssertTrue(model.hasFinalEntries)
        XCTAssertNil(model.jobToken)
        XCTAssertEqual(model.editor.entries.count, 1)
        XCTAssertEqual(model.transcriptAccessMode, .editable)
    }

    func testIncrementalEntriesDoNotMarkHasFinalEntries() {
        let model = makeModel()
        let url = URL(fileURLWithPath: "/tmp/test.mp4")
        model.openVideo(url: url)
        let sessionToken = model.sessionToken
        model.handleMetadataLoaded(VideoMetadata(fileName: "test.mp4", fileSize: 100, width: 1920, height: 1080, durationMs: 1000, codec: "h264"), sessionToken: sessionToken)

        model.startExtraction()
        let jobToken = model.jobToken!

        model.handleExtractionIncrementalEntries(
            [SubtitleEntryData(startMs: 0, endMs: 500, text: "Incremental", confidence: 0.8)],
            jobToken: jobToken
        )
        XCTAssertEqual(model.editor.entries.count, 1)
        XCTAssertFalse(model.hasFinalEntries)

        model.cancelExtraction()
        XCTAssertEqual(model.state, .cancelled)
        XCTAssertFalse(model.hasFinalEntries)
        XCTAssertEqual(model.transcriptAccessMode, .readOnly)
        XCTAssertFalse(model.commandAvailability.canExport)
        XCTAssertFalse(model.commandAvailability.canEdit)
    }

    func testOutOfOrderExtractionEventsDoNotRegressState() {
        let model = makeModel()
        let url = URL(fileURLWithPath: "/tmp/test.mp4")
        model.openVideo(url: url)
        let sessionToken = model.sessionToken
        model.handleMetadataLoaded(VideoMetadata(fileName: "test.mp4", fileSize: 100, width: 1920, height: 1080, durationMs: 1000, codec: "h264"), sessionToken: sessionToken)

        model.startExtraction()
        let jobToken = model.jobToken!

        model.handleExtractionFinalizing(jobToken: jobToken)
        XCTAssertEqual(model.state, .finalizing)

        // 迟到的 progress 不应导致 finalizing 状态倒退回 processing
        model.handleExtractionProgress(progress: 0.3, frameCount: 3, totalFrames: 10, jobToken: jobToken)
        XCTAssertEqual(model.state, .finalizing)
    }

    func testProcessingAndFinalizingTranscriptIsReadOnlyAndCommandsDisabled() {
        let model = makeModel()
        let url = URL(fileURLWithPath: "/tmp/test.mp4")
        model.openVideo(url: url)
        let sessionToken = model.sessionToken
        model.handleMetadataLoaded(VideoMetadata(fileName: "test.mp4", fileSize: 100, width: 1920, height: 1080, durationMs: 1000, codec: "h264"), sessionToken: sessionToken)

        model.startExtraction()
        let jobToken = model.jobToken!

        model.handleExtractionProgress(progress: 0.2, frameCount: 2, totalFrames: 10, jobToken: jobToken)
        XCTAssertEqual(model.transcriptAccessMode, .readOnly)
        XCTAssertFalse(model.commandAvailability.canEdit)
        XCTAssertFalse(model.commandAvailability.canSplit)
        XCTAssertFalse(model.commandAvailability.canMerge)
        XCTAssertFalse(model.commandAvailability.canExport)
        XCTAssertTrue(model.commandAvailability.canStop)
        XCTAssertFalse(model.commandAvailability.canExtract)

        model.handleExtractionFinalizing(jobToken: jobToken)
        XCTAssertEqual(model.transcriptAccessMode, .readOnly)
        XCTAssertFalse(model.commandAvailability.canExport)
    }

    func testReviewCommandsAreEnabled() {
        let model = makeModel()
        let url = URL(fileURLWithPath: "/tmp/test.mp4")
        model.openVideo(url: url)
        let sessionToken = model.sessionToken
        model.handleMetadataLoaded(VideoMetadata(fileName: "test.mp4", fileSize: 100, width: 1920, height: 1080, durationMs: 1000, codec: "h264"), sessionToken: sessionToken)

        model.startExtraction()
        let jobToken = model.jobToken!
        let entriesData = [
            SubtitleEntryData(startMs: 0, endMs: 500, text: "Line 1", confidence: 0.9),
            SubtitleEntryData(startMs: 500, endMs: 1000, text: "Line 2", confidence: 0.9)
        ]
        model.handleExtractionFinalEntries(entriesData, jobToken: jobToken)

        XCTAssertEqual(model.state, .review)
        XCTAssertEqual(model.transcriptAccessMode, .editable)
        XCTAssertTrue(model.commandAvailability.canExport)
        XCTAssertTrue(model.commandAvailability.canEdit)

        // 没选选中条目时 split/merge 禁用
        XCTAssertNil(model.editor.selectedId)
        XCTAssertFalse(model.commandAvailability.canSplit)
        XCTAssertFalse(model.commandAvailability.canMerge)

        // 选中第一条：split 可用，merge 可用
        model.selectSubtitle(id: model.editor.entries.first?.id)
        XCTAssertTrue(model.commandAvailability.canSplit)
        XCTAssertTrue(model.commandAvailability.canMerge)

        // 选中最后一条：split 可用，merge 禁用（因为之后没有下一条可合并）
        model.selectSubtitle(id: model.editor.entries.last?.id)
        XCTAssertTrue(model.commandAvailability.canSplit)
        XCTAssertFalse(model.commandAvailability.canMerge)
    }

    func testNewVideoCleansOldSessionStateWithoutChangingSettings() {
        let model = makeModel()
        let url1 = URL(fileURLWithPath: "/tmp/video1.mp4")
        model.openVideo(url: url1)
        let token1 = model.sessionToken
        model.handleMetadataLoaded(VideoMetadata(fileName: "video1.mp4", fileSize: 100, width: 1920, height: 1080, durationMs: 1000, codec: "h264"), sessionToken: token1)

        model.startExtraction()
        let jobToken1 = model.jobToken!
        model.handleExtractionFinalEntries([SubtitleEntryData(startMs: 0, endMs: 100, text: "Old", confidence: 1.0)], jobToken: jobToken1)
        model.selectSubtitle(id: model.editor.entries.first?.id)

        XCTAssertEqual(model.editor.entries.count, 1)
        XCTAssertTrue(model.hasFinalEntries)

        let url2 = URL(fileURLWithPath: "/tmp/video2.mp4")
        model.openVideo(url: url2)

        XCTAssertNotEqual(model.sessionToken, token1)
        XCTAssertNil(model.jobToken)
        XCTAssertEqual(model.state, .loading)
        XCTAssertFalse(model.hasFinalEntries)
        XCTAssertTrue(model.editor.entries.isEmpty)
        XCTAssertNil(model.editor.selectedId)
        XCTAssertNil(model.errorMessage)
    }

    func testCancelAndRestart() {
        let model = makeModel()
        let url = URL(fileURLWithPath: "/tmp/test.mp4")
        model.openVideo(url: url)
        let token = model.sessionToken
        model.handleMetadataLoaded(VideoMetadata(fileName: "test.mp4", fileSize: 100, width: 1920, height: 1080, durationMs: 1000, codec: "h264"), sessionToken: token)

        model.startExtraction()
        XCTAssertEqual(model.state, .starting)
        model.cancelExtraction()

        XCTAssertEqual(model.state, .cancelled)
        XCTAssertNil(model.jobToken)
        XCTAssertTrue(model.commandAvailability.canExtract)

        model.startExtraction()
        XCTAssertEqual(model.state, .starting)
    }

    func testErrorAndRetry() {
        let model = makeModel()
        let url = URL(fileURLWithPath: "/tmp/test.mp4")
        model.openVideo(url: url)
        let sessionToken = model.sessionToken
        model.handleMetadataLoaded(VideoMetadata(fileName: "test.mp4", fileSize: 100, width: 1920, height: 1080, durationMs: 1000, codec: "h264"), sessionToken: sessionToken)

        model.startExtraction()
        let jobToken = model.jobToken!
        model.handleExtractionError("Server Crashed", jobToken: jobToken)

        XCTAssertEqual(model.state, .failed)
        XCTAssertEqual(model.errorMessage, "Server Crashed")

        model.retry()
        XCTAssertEqual(model.state, .ready)
        XCTAssertNil(model.errorMessage)
    }

    func testStaleSessionTokenIgnored() {
        let model = makeModel()
        let url1 = URL(fileURLWithPath: "/tmp/video1.mp4")
        model.openVideo(url: url1)
        let oldToken = model.sessionToken

        model.handleMetadataLoaded(VideoMetadata(fileName: "video1.mp4", fileSize: 100, width: 1920, height: 1080, durationMs: 1000, codec: "h264"), sessionToken: oldToken)
        XCTAssertEqual(model.state, .ready)

        let url2 = URL(fileURLWithPath: "/tmp/video2.mp4")
        model.openVideo(url: url2)
        let newToken = model.sessionToken

        model.handleMetadataLoaded(VideoMetadata(fileName: "video1.mp4", fileSize: 100, width: 1920, height: 1080, durationMs: 1000, codec: "h264"), sessionToken: oldToken)
        XCTAssertEqual(model.state, .loading)

        model.handleMetadataLoaded(VideoMetadata(fileName: "video2.mp4", fileSize: 200, width: 1920, height: 1080, durationMs: 2000, codec: "h264"), sessionToken: newToken)
        XCTAssertEqual(model.state, .ready)
    }

    func testStaleJobTokenIgnored() {
        let model = makeModel()
        let url = URL(fileURLWithPath: "/tmp/test.mp4")
        model.openVideo(url: url)
        let sessionToken = model.sessionToken
        model.handleMetadataLoaded(VideoMetadata(fileName: "test.mp4", fileSize: 100, width: 1920, height: 1080, durationMs: 1000, codec: "h264"), sessionToken: sessionToken)

        model.startExtraction()
        let staleJobToken = model.jobToken!

        model.cancelExtraction()
        XCTAssertEqual(model.state, .cancelled)

        model.handleExtractionProgress(progress: 0.9, frameCount: 9, totalFrames: 10, jobToken: staleJobToken)
        XCTAssertEqual(model.state, .cancelled)

        model.handleExtractionFinalEntries([SubtitleEntryData(startMs: 0, endMs: 100, text: "Stale", confidence: 1.0)], jobToken: staleJobToken)
        XCTAssertEqual(model.state, .cancelled)
        XCTAssertTrue(model.editor.entries.isEmpty)
    }

    func testInspectorIsNotForcedOpenOnStateTransitions() {
        let model = makeModel()
        XCTAssertFalse(model.isInspectorPresented)

        let url = URL(fileURLWithPath: "/tmp/test.mp4")
        model.openVideo(url: url)
        XCTAssertFalse(model.isInspectorPresented)

        let token = model.sessionToken
        model.handleMetadataLoaded(VideoMetadata(fileName: "test.mp4", fileSize: 100, width: 1920, height: 1080, durationMs: 1000, codec: "h264"), sessionToken: token)
        XCTAssertFalse(model.isInspectorPresented)

        model.startExtraction()
        XCTAssertFalse(model.isInspectorPresented)

        model.handleExtractionFinalEntries([SubtitleEntryData(startMs: 0, endMs: 100, text: "Hi", confidence: 1.0)], jobToken: model.jobToken!)
        XCTAssertFalse(model.isInspectorPresented)
    }

    func testCommandAvailabilityMatrixConsistency() {
        // empty
        let emptyAvail = WorkspaceCommandAvailability.derive(state: .empty, hasFinalEntries: false)
        XCTAssertTrue(emptyAvail.canOpen)
        XCTAssertFalse(emptyAvail.canEditRegion)
        XCTAssertFalse(emptyAvail.canExtract)
        XCTAssertFalse(emptyAvail.canShowInspector)

        // ready
        let readyAvail = WorkspaceCommandAvailability.derive(state: .ready, hasFinalEntries: false)
        XCTAssertTrue(readyAvail.canOpen)
        XCTAssertTrue(readyAvail.canEditRegion)
        XCTAssertTrue(readyAvail.canExtract)
        XCTAssertFalse(readyAvail.canStop)
        XCTAssertFalse(readyAvail.canExport)
        XCTAssertTrue(readyAvail.canShowInspector)

        // processing
        let processingAvail = WorkspaceCommandAvailability.derive(state: .processing, hasFinalEntries: false)
        XCTAssertFalse(processingAvail.canOpen)
        XCTAssertFalse(processingAvail.canEditRegion)
        XCTAssertFalse(processingAvail.canExtract)
        XCTAssertTrue(processingAvail.canStop)
        XCTAssertFalse(processingAvail.canExport)
        XCTAssertFalse(processingAvail.canEdit)

        // review with final entries and valid selection
        let reviewAvail = WorkspaceCommandAvailability.derive(state: .review, hasFinalEntries: true, totalEntries: 2, selectedIndex: 0)
        XCTAssertTrue(reviewAvail.canOpen)
        XCTAssertTrue(reviewAvail.canEditRegion)
        XCTAssertTrue(reviewAvail.canExtract)
        XCTAssertTrue(reviewAvail.canExport)
        XCTAssertTrue(reviewAvail.canEdit)
        XCTAssertTrue(reviewAvail.canSplit)
        XCTAssertTrue(reviewAvail.canMerge)
    }

    func testOpenVideoRejectsIllegalLoadingAndProcessingTransitions() {
        let model = makeModel()
        let firstURL = URL(fileURLWithPath: "/tmp/first.mp4")
        let secondURL = URL(fileURLWithPath: "/tmp/second.mp4")

        XCTAssertTrue(model.openVideo(url: firstURL))
        let firstSessionToken = model.sessionToken
        XCTAssertFalse(model.openVideo(url: secondURL))
        XCTAssertEqual(model.currentVideoURL, firstURL)
        XCTAssertEqual(model.sessionToken, firstSessionToken)
        XCTAssertEqual(model.state, .loading)

        model.handleMetadataLoaded(
            VideoMetadata(fileName: "first.mp4", fileSize: 100, width: 1920, height: 1080, durationMs: 1000, codec: "h264"),
            sessionToken: firstSessionToken
        )
        model.startExtraction()
        let jobToken = model.jobToken!
        model.handleExtractionProgress(progress: 0.2, frameCount: 2, totalFrames: 10, jobToken: jobToken)

        XCTAssertFalse(model.openVideo(url: secondURL))
        XCTAssertEqual(model.currentVideoURL, firstURL)
        XCTAssertEqual(model.jobToken, jobToken)
        XCTAssertEqual(model.state, .processing)
        model.cancelExtraction()
    }

    func testReExtractionFailureRestoresLastCompletedResult() {
        let model = makeModel()
        let url = URL(fileURLWithPath: "/tmp/test.mp4")
        model.openVideo(url: url)
        let sessionToken = model.sessionToken
        model.handleMetadataLoaded(
            VideoMetadata(fileName: "test.mp4", fileSize: 100, width: 1920, height: 1080, durationMs: 1000, codec: "h264"),
            sessionToken: sessionToken
        )

        model.startExtraction()
        let firstJobToken = model.jobToken!
        let completed = [SubtitleEntryData(startMs: 0, endMs: 500, text: "Completed", confidence: 0.9)]
        model.handleExtractionFinalEntries(completed, jobToken: firstJobToken)

        model.startExtraction()
        let retryJobToken = model.jobToken!
        model.handleExtractionIncrementalEntries(
            [SubtitleEntryData(startMs: 0, endMs: 500, text: "Temporary", confidence: 0.5)],
            jobToken: retryJobToken
        )
        model.handleExtractionError("提取失败", jobToken: retryJobToken)

        XCTAssertEqual(model.state, .failed)
        XCTAssertTrue(model.hasFinalEntries)
        XCTAssertEqual(model.editor.exportEntries(), completed)
        XCTAssertTrue(model.commandAvailability.canExport)

        model.retry()
        XCTAssertEqual(model.state, .review)
    }

    func testReExtractionCancellationRestoresLastCompletedResult() {
        let model = makeModel()
        let url = URL(fileURLWithPath: "/tmp/test.mp4")
        model.openVideo(url: url)
        let sessionToken = model.sessionToken
        model.handleMetadataLoaded(
            VideoMetadata(fileName: "test.mp4", fileSize: 100, width: 1920, height: 1080, durationMs: 1000, codec: "h264"),
            sessionToken: sessionToken
        )

        model.startExtraction()
        let firstJobToken = model.jobToken!
        let completed = [SubtitleEntryData(startMs: 0, endMs: 500, text: "Completed", confidence: 0.9)]
        model.handleExtractionFinalEntries(completed, jobToken: firstJobToken)

        model.startExtraction()
        model.cancelExtraction()

        XCTAssertEqual(model.state, .cancelled)
        XCTAssertTrue(model.hasFinalEntries)
        XCTAssertEqual(model.editor.exportEntries(), completed)
        XCTAssertTrue(model.commandAvailability.canExport)
    }
}
