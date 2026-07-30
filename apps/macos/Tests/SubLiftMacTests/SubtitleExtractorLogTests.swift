import XCTest
@testable import SubLiftMac

final class SubtitleExtractorLogTests: XCTestCase {
    func testCppPathModeLogFieldsNameCppBackendAndExtractor() {
        let fields = SubtitleExtractor.pathModeLogFields(
            for: WorkerChoice(runtime: .cpp, engine: .paddle, resolvedVia: .envVar)
        )

        XCTAssertEqual(fields.backend, "C++")
        XCTAssertEqual(fields.extractor, "C++ FfmpegExtractor")
    }

    func testPythonPathModeLogFieldsNamePythonBackendAndExtractor() {
        let fields = SubtitleExtractor.pathModeLogFields(
            for: WorkerChoice(runtime: .python, engine: .paddle, resolvedVia: .explicitFlag)
        )

        XCTAssertEqual(fields.backend, "Python")
        XCTAssertEqual(fields.extractor, "Python FfmpegExtractor")
    }

    func testMissingWorkerChoiceDoesNotClaimPythonRuntime() {
        let fields = SubtitleExtractor.pathModeLogFields(for: nil)

        XCTAssertEqual(fields.backend, "unknown")
        XCTAssertEqual(fields.extractor, "unknown extractor")
    }
}
