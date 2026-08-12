import XCTest
@testable import SubLiftMac

/// 10311：引擎可见性纯逻辑。
final class EngineCapabilityTests: XCTestCase {

    func testMockOnlyVisibleInDeveloperMode() {
        XCTAssertFalse(EngineCapability.visibleEngines(developerMode: false).contains(.mock))
        XCTAssertTrue(EngineCapability.visibleEngines(developerMode: true).contains(.mock))
        // vision/paddle 始终可见。
        XCTAssertTrue(EngineCapability.visibleEngines(developerMode: false).contains(.vision))
        XCTAssertTrue(EngineCapability.visibleEngines(developerMode: false).contains(.paddle))
    }

    func testPersistedMockFallsBackToVisionOutsideDeveloperMode() {
        XCTAssertEqual(
            EngineCapability.normalizedSelection(.mock, developerMode: false),
            .vision
        )
    }

    func testPersistedMockRemainsSelectedInDeveloperMode() {
        XCTAssertEqual(
            EngineCapability.normalizedSelection(.mock, developerMode: true),
            .mock
        )
    }

    func testVisibleProductionSelectionIsPreserved() {
        XCTAssertEqual(
            EngineCapability.normalizedSelection(.paddle, developerMode: false),
            .paddle
        )
    }

    func testNormalizedSelectionAlwaysBelongsToVisibleEngines() {
        for developerMode in [false, true] {
            for engine in OcrEngineName.allCases {
                let normalized = EngineCapability.normalizedSelection(
                    engine,
                    developerMode: developerMode
                )
                XCTAssertTrue(
                    EngineCapability.visibleEngines(developerMode: developerMode).contains(normalized),
                    "\(normalized) must be visible when developerMode=\(developerMode)"
                )
            }
        }
    }

    func testNoAutomaticOrWhisperEngines() {
        let all = EngineCapability.visibleEngines(developerMode: true)
        XCTAssertFalse(all.contains { $0.rawValue.contains("automatic") })
        XCTAssertFalse(all.contains { $0.rawValue.contains("whisper") })
    }

    func testVisionAvailableWithCppRuntime() {
        let status = EngineCapability.status(
            for: .vision,
            ffmpegAvailable: true,
            paddleModelsAvailable: false,
            developerMode: false
        )
        XCTAssertTrue(status.available)
        XCTAssertTrue(status.summary.contains("可用"))
    }

    func testPaddleUnavailableWithoutModels() {
        let status = EngineCapability.status(
            for: .paddle,
            ffmpegAvailable: true,
            paddleModelsAvailable: false,
            developerMode: false
        )
        XCTAssertFalse(status.available)
        XCTAssertTrue(status.detail?.contains("模型未安装") == true)
    }

    func testPaddleAvailableWithModels() {
        let status = EngineCapability.status(
            for: .paddle,
            ffmpegAvailable: true,
            paddleModelsAvailable: true,
            developerMode: false
        )
        XCTAssertTrue(status.available)
    }

    func testMockUnavailableOutsideDeveloperMode() {
        let status = EngineCapability.status(
            for: .mock,
            ffmpegAvailable: true,
            paddleModelsAvailable: false,
            developerMode: false
        )
        XCTAssertFalse(status.available)
        XCTAssertTrue(status.summary.contains("开发者模式"))
    }

    func testFfmpegMissingReflectedInline() {
        // ffmpeg 缺失：capability 检测如实反映（MKV/回退预览不可用）。
        let status = EngineCapability.status(
            for: .vision,
            ffmpegAvailable: false,
            paddleModelsAvailable: false,
            developerMode: false
        )
        XCTAssertFalse(status.detail?.contains("ffmpeg") == false, "缺 ffmpeg 应有 inline 说明")
    }

    func testRuntimeLabelUsesRealResolution() {
        // Python 仅显式 Oracle/回滚：默认解析为 C++ runtime。
        let label = EngineCapability.runtimeLabel(
            for: .vision,
            ffmpegAvailable: true,
            paddleModelsAvailable: true
        )
        XCTAssertTrue(label.contains("C++"), "默认 runtime 应为 C++：\(label)")
    }
}

/// 10311：Settings 面板结构（三分区、无假下载按钮）。
final class SettingsPresentationTests: XCTestCase {

    func testThreeSections() {
        let sections = SettingsPresentation.sections
        XCTAssertEqual(sections, ["General", "Recognition", "Advanced"])
    }

    func testNoFakeModelDownloadAction() {
        // 面板不提供任何"下载模型"按钮/动作。
        XCTAssertFalse(SettingsPresentation.actions.contains { $0.contains("下载") })
    }
}
