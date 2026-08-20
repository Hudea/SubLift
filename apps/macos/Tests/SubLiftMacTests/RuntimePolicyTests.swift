import XCTest
@testable import SubLiftMac

final class RuntimePolicyTests: XCTestCase {
    func testDefaultCppRuntime() throws {
        let choice = try RuntimePolicy.resolve(
            requestedEngine: "vision",
            envOverride: [:]
        )
        XCTAssertEqual(choice, WorkerChoice(runtime: .cpp, engine: .vision, resolvedVia: .productDefault))
    }

    func testEnvVarCppIsIgnoredNoOp() throws {
        let choice = try RuntimePolicy.resolve(
            requestedEngine: "vision",
            envOverride: ["SUBLIFT_RUNTIME": "cpp"]
        )
        XCTAssertEqual(choice, WorkerChoice(runtime: .cpp, engine: .vision, resolvedVia: .productDefault))
    }

    func testPythonEnvFailsClosed() {
        XCTAssertThrowsError(
            try RuntimePolicy.resolve(
                requestedEngine: "vision",
                envOverride: ["SUBLIFT_RUNTIME": "python"]
            )
        ) { error in
            XCTAssertEqual(error as? RuntimePolicyError, RuntimePolicyError.pythonRuntimeRequested)
        }
    }

    func testPaddleEngineThrowsWhenCppUnavailable() {
        XCTAssertThrowsError(
            try RuntimePolicy.resolve(
                requestedEngine: "paddle",
                envOverride: [:],
                isCppPaddleAvailable: false
            )
        ) { error in
            XCTAssertEqual(error as? RuntimePolicyError, RuntimePolicyError.paddleCppUnavailable)
        }
    }

    func testPaddleProductDefaultUsesCppWhenAvailable() throws {
        let choice = try RuntimePolicy.resolve(
            requestedEngine: "paddle",
            envOverride: [:],
            isCppPaddleAvailable: true
        )
        XCTAssertEqual(choice, WorkerChoice(runtime: .cpp, engine: .paddle, resolvedVia: .productDefault))
    }

    func testPaddlePythonEnvFailsClosedEvenIfCppAvailable() {
        XCTAssertThrowsError(
            try RuntimePolicy.resolve(
                requestedEngine: "paddle",
                envOverride: ["SUBLIFT_RUNTIME": "python"],
                isCppPaddleAvailable: true
            )
        ) { error in
            XCTAssertEqual(error as? RuntimePolicyError, RuntimePolicyError.pythonRuntimeRequested)
        }
    }

    func testMockEngineNativeOnly() throws {
        let choice = try RuntimePolicy.resolve(
            requestedEngine: "mock",
            envOverride: [:]
        )
        XCTAssertEqual(choice, WorkerChoice(runtime: .cpp, engine: .mock, resolvedVia: .productDefault))
    }

    func testWhitespaceAndCaseTrimming() throws {
        let choice = try RuntimePolicy.resolve(
            requestedEngine: "  VISION ",
            envOverride: ["SUBLIFT_RUNTIME": "  CPP  "]
        )
        XCTAssertEqual(choice, WorkerChoice(runtime: .cpp, engine: .vision, resolvedVia: .productDefault))
    }

    func testInvalidEngineThrows() {
        XCTAssertThrowsError(
            try RuntimePolicy.resolve(requestedEngine: "onnx")
        ) { error in
            XCTAssertEqual(error as? RuntimePolicyError, RuntimePolicyError.unsupportedEngine("onnx"))
        }
    }

    func testInvalidRuntimeEnvThrows() {
        XCTAssertThrowsError(
            try RuntimePolicy.resolve(
                requestedEngine: "vision",
                envOverride: ["SUBLIFT_RUNTIME": "invalid_rt"]
            )
        ) { error in
            XCTAssertEqual(error as? RuntimePolicyError, RuntimePolicyError.invalidRuntime("invalid_rt"))
        }
    }
}
