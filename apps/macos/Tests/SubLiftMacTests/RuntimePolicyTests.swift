import XCTest
@testable import SubLiftMac

final class RuntimePolicyTests: XCTestCase {
    func testDefaultCppRuntime() throws {
        let choice = try RuntimePolicy.resolve(
            requestedRuntime: nil,
            requestedEngine: "vision",
            envOverride: [:]
        )
        XCTAssertEqual(choice, WorkerChoice(runtime: .cpp, engine: .vision, resolvedVia: .productDefault))
    }

    func testEnvVarCppOverride() throws {
        let choice = try RuntimePolicy.resolve(
            requestedRuntime: nil,
            requestedEngine: "vision",
            envOverride: ["SUBLIFT_RUNTIME": "cpp"],
            defaultRuntime: .python
        )
        XCTAssertEqual(choice, WorkerChoice(runtime: .cpp, engine: .vision, resolvedVia: .envVar))
    }

    func testExplicitFlagOverridesEnv() throws {
        let choice = try RuntimePolicy.resolve(
            requestedRuntime: "python",
            requestedEngine: "vision",
            envOverride: ["SUBLIFT_RUNTIME": "cpp"],
            defaultRuntime: .python
        )
        XCTAssertEqual(choice, WorkerChoice(runtime: .python, engine: .vision, resolvedVia: .explicitFlag))
    }

    func testPaddleEngineForcesPythonRuntime() throws {
        let choice = try RuntimePolicy.resolve(
            requestedRuntime: "cpp",
            requestedEngine: "paddle",
            envOverride: ["SUBLIFT_RUNTIME": "cpp"],
            defaultRuntime: .python
        )
        XCTAssertEqual(choice, WorkerChoice(runtime: .python, engine: .paddle, resolvedVia: .paddleOverride))
    }

    func testMockEngineEnvCpp() throws {
        let choice = try RuntimePolicy.resolve(
            requestedRuntime: nil,
            requestedEngine: "mock",
            envOverride: ["SUBLIFT_RUNTIME": "cpp"],
            defaultRuntime: .python
        )
        XCTAssertEqual(choice, WorkerChoice(runtime: .cpp, engine: .mock, resolvedVia: .envVar))
    }

    func testWhitespaceAndCaseTrimming() throws {
        let choice = try RuntimePolicy.resolve(
            requestedRuntime: "  CPP  ",
            requestedEngine: "  VISION ",
            envOverride: [:],
            defaultRuntime: .python
        )
        XCTAssertEqual(choice, WorkerChoice(runtime: .cpp, engine: .vision, resolvedVia: .explicitFlag))
    }

    func testCutoverDefaultCpp() throws {
        let choice = try RuntimePolicy.resolve(
            requestedRuntime: nil,
            requestedEngine: "vision",
            envOverride: [:],
            defaultRuntime: .cpp
        )
        XCTAssertEqual(choice, WorkerChoice(runtime: .cpp, engine: .vision, resolvedVia: .productDefault))
    }

    func testInvalidEngineThrows() {
        XCTAssertThrowsError(
            try RuntimePolicy.resolve(requestedRuntime: nil, requestedEngine: "onnx")
        ) { error in
            XCTAssertEqual(error as? RuntimePolicyError, RuntimePolicyError.unsupportedEngine("onnx"))
        }
    }

    func testInvalidRuntimeThrows() {
        XCTAssertThrowsError(
            try RuntimePolicy.resolve(requestedRuntime: "invalid_rt", requestedEngine: "vision")
        ) { error in
            XCTAssertEqual(error as? RuntimePolicyError, RuntimePolicyError.invalidRuntime("invalid_rt"))
        }
    }
}
