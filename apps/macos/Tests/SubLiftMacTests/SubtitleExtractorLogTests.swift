import XCTest
@testable import SubLiftMac

final class SubtitleExtractorLogTests: XCTestCase {
    func testCppPathModeLogFieldsNameCppBackendAndExtractor() {
        let fields = SubtitleExtractor.pathModeLogFields(
            for: WorkerChoice(runtime: .cpp, engine: .paddle, resolvedVia: .productDefault)
        )

        XCTAssertEqual(fields.backend, "C++")
        XCTAssertEqual(fields.extractor, "C++ FfmpegExtractor")
    }

    func testMissingWorkerChoiceDoesNotClaimPythonRuntime() {
        let fields = SubtitleExtractor.pathModeLogFields(for: nil)

        XCTAssertEqual(fields.backend, "unknown")
        XCTAssertEqual(fields.extractor, "unknown extractor")
    }
}
