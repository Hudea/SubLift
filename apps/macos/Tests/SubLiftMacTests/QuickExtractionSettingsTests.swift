import XCTest
@testable import SubLiftMac

// MARK: - ExtractionConfiguration 相等性（10415）

final class ExtractionConfigurationTests: XCTestCase {

    func testEqualitySameEngineAndQuality() {
        let a = ExtractionConfiguration(engine: .vision, quality: .fast)
        let b = ExtractionConfiguration(engine: .vision, quality: .fast)
        XCTAssertEqual(a, b)
    }

    func testInequalityWhenEngineDiffers() {
        let a = ExtractionConfiguration(engine: .vision, quality: .fast)
        let b = ExtractionConfiguration(engine: .paddle, quality: .fast)
        XCTAssertNotEqual(a, b)
    }

    func testInequalityWhenQualityDiffers() {
        let a = ExtractionConfiguration(engine: .vision, quality: .fast)
        let b = ExtractionConfiguration(engine: .vision, quality: .fine)
        XCTAssertNotEqual(a, b)
    }
}

// MARK: - Workspace 配置快照生命周期（10415）

@MainActor
final class WorkspaceConfigurationSnapshotTests: XCTestCase {

    private func makeModel() -> WorkspaceModel {
        WorkspaceModel(metadataRequest: { url in
            VideoMetadata(
                fileName: url.lastPathComponent,
                fileSize: 0,
                width: 0,
                height: 0,
                durationMs: 0,
                codec: "unknown"
            )
        })
    }

    /// 打开视频并进入 ready。
    private func openReadyModel() -> WorkspaceModel {
        let model = makeModel()
        let url = URL(fileURLWithPath: "/tmp/test.mp4")
        model.openVideo(url: url)
        let sessionToken = model.sessionToken
        model.handleMetadataLoaded(
            VideoMetadata(fileName: "test.mp4", fileSize: 100, width: 1920, height: 1080, durationMs: 1000, codec: "h264"),
            sessionToken: sessionToken
        )
        return model
    }

    func testActiveConfigurationSetAtStartup() {
        let model = openReadyModel()
        model.startExtraction(engine: .paddle, quality: .fine)
        XCTAssertEqual(
            model.activeExtractionConfiguration,
            ExtractionConfiguration(engine: .paddle, quality: .fine)
        )
        XCTAssertNil(model.finalExtractionConfiguration)
    }

    func testActiveClearedAndFinalUpdatedOnSuccess() {
        let model = openReadyModel()
        model.startExtraction(engine: .vision, quality: .balanced)
        let jobToken = model.jobToken!
        let entries = [SubtitleEntryData(startMs: 0, endMs: 1000, text: "OK", confidence: 0.9)]
        model.handleExtractionFinalEntries(entries, jobToken: jobToken)

        XCTAssertEqual(model.state, .review)
        XCTAssertNil(model.activeExtractionConfiguration)
        XCTAssertEqual(
            model.finalExtractionConfiguration,
            ExtractionConfiguration(engine: .vision, quality: .balanced)
        )
    }

    func testActiveClearedOnFailure() {
        let model = openReadyModel()
        model.startExtraction(engine: .vision, quality: .fast)
        let jobToken = model.jobToken!
        model.handleExtractionError("提取失败", jobToken: jobToken)

        XCTAssertEqual(model.state, .failed)
        XCTAssertNil(model.activeExtractionConfiguration)
        XCTAssertNil(model.finalExtractionConfiguration, "无旧结果时 final 快照保持 nil")
    }

    func testActiveClearedOnCancel() {
        let model = openReadyModel()
        model.startExtraction(engine: .vision, quality: .fast)
        model.cancelExtraction()

        XCTAssertEqual(model.state, .cancelled)
        XCTAssertNil(model.activeExtractionConfiguration)
    }

    func testFinalOnlyUpdatedOnSuccess() {
        let model = openReadyModel()
        model.startExtraction(engine: .vision, quality: .fast)
        let firstJob = model.jobToken!
        model.handleExtractionFinalEntries(
            [SubtitleEntryData(startMs: 0, endMs: 1000, text: "A", confidence: 0.9)],
            jobToken: firstJob
        )
        let finalAfterFirst = model.finalExtractionConfiguration

        // 第二次提取失败：final 快照不得被覆盖。
        model.startExtraction(engine: .paddle, quality: .fine)
        let secondJob = model.jobToken!
        model.handleExtractionError("失败", jobToken: secondJob)

        XCTAssertEqual(model.finalExtractionConfiguration, finalAfterFirst)
        XCTAssertEqual(model.state, .failed)
        XCTAssertTrue(model.hasFinalEntries)
    }

    func testFailedReextractionKeepsOldFinalSnapshot() {
        let model = openReadyModel()
        model.startExtraction(engine: .vision, quality: .fast)
        let firstJob = model.jobToken!
        model.handleExtractionFinalEntries(
            [SubtitleEntryData(startMs: 0, endMs: 1000, text: "Old", confidence: 0.9)],
            jobToken: firstJob
        )

        model.startExtraction(engine: .paddle, quality: .fine)
        let retryJob = model.jobToken!
        model.handleExtractionError("重提取失败", jobToken: retryJob)

        XCTAssertEqual(
            model.finalExtractionConfiguration,
            ExtractionConfiguration(engine: .vision, quality: .fast),
            "失败恢复旧字幕时必须保留旧 final 配置快照"
        )
        XCTAssertNil(model.activeExtractionConfiguration)
        XCTAssertEqual(model.editor.exportEntries().count, 1)
    }

    func testCancelledReextractionKeepsOldFinalSnapshot() {
        let model = openReadyModel()
        model.startExtraction(engine: .vision, quality: .fast)
        let firstJob = model.jobToken!
        model.handleExtractionFinalEntries(
            [SubtitleEntryData(startMs: 0, endMs: 1000, text: "Old", confidence: 0.9)],
            jobToken: firstJob
        )

        model.startExtraction(engine: .paddle, quality: .fine)
        model.cancelExtraction()

        XCTAssertEqual(
            model.finalExtractionConfiguration,
            ExtractionConfiguration(engine: .vision, quality: .fast)
        )
        XCTAssertNil(model.activeExtractionConfiguration)
    }

    func testOpenVideoClearsSnapshots() {
        let model = openReadyModel()
        model.startExtraction(engine: .vision, quality: .fast)
        let jobToken = model.jobToken!
        model.handleExtractionFinalEntries(
            [SubtitleEntryData(startMs: 0, endMs: 1000, text: "A", confidence: 0.9)],
            jobToken: jobToken
        )
        XCTAssertNotNil(model.finalExtractionConfiguration)

        model.openVideo(url: URL(fileURLWithPath: "/tmp/other.mp4"))

        XCTAssertNil(model.activeExtractionConfiguration)
        XCTAssertNil(model.finalExtractionConfiguration)
        XCTAssertFalse(model.hasFinalEntries)
    }
}

// MARK: - 快速设置栏展示纯逻辑（10415）

final class QuickExtractionSettingsPresentationTests: XCTestCase {

    private let preferences = ExtractionConfiguration(engine: .vision, quality: .fast)

    func testNoPendingNoticeWithoutFinalConfiguration() {
        XCTAssertFalse(QuickExtractionSettingsPresentation.showsPendingNotice(
            preferences: preferences,
            finalConfiguration: nil,
            state: .review
        ))
    }

    func testNoPendingNoticeWhenPreferencesMatchFinal() {
        XCTAssertFalse(QuickExtractionSettingsPresentation.showsPendingNotice(
            preferences: preferences,
            finalConfiguration: preferences,
            state: .review
        ))
    }

    func testPendingNoticeWhenPreferencesDiffer() {
        let different = ExtractionConfiguration(engine: .vision, quality: .fine)
        XCTAssertTrue(QuickExtractionSettingsPresentation.showsPendingNotice(
            preferences: preferences,
            finalConfiguration: different,
            state: .review
        ))
    }

    func testNoPendingNoticeWhileRunning() {
        // 运行中任务已按启动时快照运行，提示会误导（P3 修复）。
        let different = ExtractionConfiguration(engine: .vision, quality: .fine)
        XCTAssertFalse(QuickExtractionSettingsPresentation.showsPendingNotice(
            preferences: preferences,
            finalConfiguration: different,
            state: .processing
        ))
    }

    func testReextractButtonOnlyWithFinalResultAndChanges() {
        let different = ExtractionConfiguration(engine: .paddle, quality: .balanced)
        // 无最终结果：不显示
        XCTAssertFalse(QuickExtractionSettingsPresentation.showsReextractButton(
            hasFinalEntries: false,
            preferences: preferences,
            finalConfiguration: different,
            state: .review
        ))
        // 有最终结果但配置未变：不显示（Toolbar 已有重新提取入口）
        XCTAssertFalse(QuickExtractionSettingsPresentation.showsReextractButton(
            hasFinalEntries: true,
            preferences: preferences,
            finalConfiguration: preferences,
            state: .review
        ))
        // 有最终结果且配置变化：显示
        XCTAssertTrue(QuickExtractionSettingsPresentation.showsReextractButton(
            hasFinalEntries: true,
            preferences: preferences,
            finalConfiguration: different,
            state: .review
        ))
    }

    func testControlsLockedWhileRunning() {
        XCTAssertTrue(QuickExtractionSettingsPresentation.isLocked(.starting))
        XCTAssertTrue(QuickExtractionSettingsPresentation.isLocked(.processing))
        XCTAssertTrue(QuickExtractionSettingsPresentation.isLocked(.finalizing))
        XCTAssertTrue(QuickExtractionSettingsPresentation.isLocked(.regionEditing), "区域编辑中提取不可达，应锁定")
        XCTAssertFalse(QuickExtractionSettingsPresentation.isLocked(.ready))
        XCTAssertFalse(QuickExtractionSettingsPresentation.isLocked(.review))
        XCTAssertFalse(QuickExtractionSettingsPresentation.isLocked(.failed))
        XCTAssertFalse(QuickExtractionSettingsPresentation.isLocked(.cancelled))
    }

    func testPendingNoticeContainsTextNotOnlyColor() {
        let notice = QuickExtractionSettingsPresentation.pendingNoticeText
        XCTAssertFalse(notice.isEmpty)
        XCTAssertTrue(notice.contains("重新提取") || notice.contains("生效"))
    }
}

// MARK: - Inspector 配置展示优先级（10415）

final class ExtractionInspectorConfigurationTests: XCTestCase {

    private let preferences = ExtractionConfiguration(engine: .vision, quality: .fast)

    func testActiveConfigurationTakesPriority() {
        let active = ExtractionConfiguration(engine: .paddle, quality: .fine)
        let final = ExtractionConfiguration(engine: .vision, quality: .balanced)
        XCTAssertEqual(
            ExtractionInspectorConfiguration.displayConfiguration(
                active: active,
                final: final,
                preferences: preferences
            ),
            active
        )
    }

    func testFinalConfigurationUsedWhenNoActive() {
        let final = ExtractionConfiguration(engine: .paddle, quality: .fine)
        XCTAssertEqual(
            ExtractionInspectorConfiguration.displayConfiguration(
                active: nil,
                final: final,
                preferences: preferences
            ),
            final
        )
    }

    func testPreferencesFallbackWhenNoSnapshots() {
        XCTAssertEqual(
            ExtractionInspectorConfiguration.displayConfiguration(
                active: nil,
                final: nil,
                preferences: preferences
            ),
            preferences
        )
    }
}

// MARK: - 提取请求策略（10415）

final class ExtractionRequestPolicyTests: XCTestCase {

    func testConfirmationRequiredOnlyWithFinalEntries() {
        XCTAssertTrue(ExtractionRequestPolicy.requiresReplacementConfirmation(hasFinalEntries: true))
        XCTAssertFalse(ExtractionRequestPolicy.requiresReplacementConfirmation(hasFinalEntries: false))
    }

    func testConfirmationMessageMentionsReplacement() {
        XCTAssertTrue(ExtractionRequestPolicy.replacementConfirmationMessage.contains("替换"))
    }

    func testMockNormalizedInRequestPath() {
        // 提取请求入口的配置冻结：非开发模式 Mock 回落 Vision（隐藏 Mock 不可运行）。
        let config = ExtractionRequestPolicy.normalizedConfiguration(
            engine: .mock,
            quality: .fine,
            developerMode: false
        )
        XCTAssertEqual(config.engine, .vision)
        XCTAssertEqual(config.quality, .fine)

        // 开发者模式保留 Mock。
        let devConfig = ExtractionRequestPolicy.normalizedConfiguration(
            engine: .mock,
            quality: .fast,
            developerMode: true
        )
        XCTAssertEqual(devConfig.engine, .mock)
    }
}
