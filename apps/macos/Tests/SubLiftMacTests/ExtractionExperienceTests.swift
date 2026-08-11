import XCTest
@testable import SubLiftMac

/// 10208：提取进度展示纯逻辑（只显示真实字段，无 ETA/假置信度）。
final class ExtractionProgressPresentationTests: XCTestCase {

    func testProgressTextUsesRealCounts() {
        let text = ExtractionProgressPresentation.progressText(pct: 0.10, frameCount: 12, totalFrames: 120)
        XCTAssertEqual(text, "12/120（10%）")
    }

    func testProgressTextHandlesZeroTotal() {
        // 总帧为 0 时不得除零/显示 NaN。
        let text = ExtractionProgressPresentation.progressText(pct: 0, frameCount: 0, totalFrames: 0)
        XCTAssertFalse(text.contains("nan"))
        XCTAssertFalse(text.contains("inf"))
    }

    func testRateTextOnlyWhenRateAvailable() {
        XCTAssertNotNil(ExtractionProgressPresentation.rateText(12.5))
        XCTAssertNil(ExtractionProgressPresentation.rateText(nil))
    }

    func testRuntimeIdentityShownWhenPresent() {
        XCTAssertEqual(
            ExtractionProgressPresentation.runtimeText(identity: "C++ worker (mock)"),
            "OCR 运行时：C++ worker (mock)"
        )
        XCTAssertNil(ExtractionProgressPresentation.runtimeText(identity: nil))
    }

    func testNoEtaOrAverageConfidenceInAnyPresentation() {
        // 不可可靠推导的 ETA/平均置信度不得出现在展示文本中。
        let samples = [
            ExtractionProgressPresentation.progressText(pct: 0.5, frameCount: 60, totalFrames: 120),
            ExtractionProgressPresentation.stageText(.startingServer) ?? "",
            ExtractionProgressPresentation.stageText(.finalizing) ?? "",
            ExtractionProgressPresentation.stageText(.done(entryCount: 5)) ?? "",
        ]
        for sample in samples {
            XCTAssertFalse(sample.contains("剩余"), "不应显示 ETA: \(sample)")
            XCTAssertFalse(sample.contains("ETA"), "不应显示 ETA: \(sample)")
        }
    }

    func testStageTextsAreRealStates() {
        XCTAssertEqual(ExtractionProgressPresentation.stageText(.startingServer), "正在启动服务…")
        XCTAssertEqual(ExtractionProgressPresentation.stageText(.finalizing), "正在整理字幕…")
        XCTAssertEqual(ExtractionProgressPresentation.stageText(.done(entryCount: 5)), "完成，识别 5 条")
        XCTAssertNil(ExtractionProgressPresentation.stageText(.idle))
    }
}

/// 10208：Live Transcript 只读说明（processing/finalizing 时提示）。
final class TranscriptReadOnlyNoticeTests: XCTestCase {

    func testProcessingStatesShowReadOnlyNotice() {
        for state in [WorkspaceState.starting, .processing, .finalizing] {
            XCTAssertNotNil(TranscriptReadOnlyNotice.text(for: state), "\(state) 应有只读说明")
        }
    }

    func testNonProcessingStatesShowNoNotice() {
        for state in [WorkspaceState.empty, .ready, .review, .failed, .cancelled] {
            XCTAssertNil(TranscriptReadOnlyNotice.text(for: state), "\(state) 不应有只读说明")
        }
    }
}

/// 10208：Extraction Inspector 展示（真实配置与 runtime）。
final class ExtractionInspectorPresentationTests: XCTestCase {

    func testRowsUseRealEngineAndQuality() {
        let rows = ExtractionInspectorPresentation.rows(
            engine: .paddle,
            quality: .fine,
            runtimeIdentity: "Python oracle (mock)",
            state: .processing
        )
        let labels = rows.map(\.label)
        XCTAssertTrue(labels.contains("引擎"))
        XCTAssertTrue(labels.contains("质量"))
        XCTAssertTrue(labels.contains("运行时"))
        XCTAssertTrue(labels.contains("状态"))

        let engineRow = rows.first { $0.label == "引擎" }
        XCTAssertEqual(engineRow?.value, "PaddleOCR")
        let qualityRow = rows.first { $0.label == "质量" }
        XCTAssertEqual(qualityRow?.value, SamplingQuality.fine.displayName)
        let runtimeRow = rows.first { $0.label == "运行时" }
        XCTAssertEqual(runtimeRow?.value, "Python oracle (mock)")
    }

    func testMissingRuntimeShowsDashNotFake() {
        let rows = ExtractionInspectorPresentation.rows(
            engine: .vision,
            quality: .fast,
            runtimeIdentity: nil,
            state: .ready
        )
        let runtimeRow = rows.first { $0.label == "运行时" }
        XCTAssertEqual(runtimeRow?.value, "—")
    }
}
