import Foundation

public enum SubLiftRuntime: String, Codable, CaseIterable, Equatable, Sendable {
    case cpp = "cpp"
}

public enum SubLiftEngine: String, Codable, CaseIterable, Equatable, Sendable {
    case vision = "vision"
    case mock = "mock"
    case paddle = "paddle"
}

public enum ResolutionSource: String, Codable, Equatable, Sendable {
    case productDefault = "product_default"
}

public struct WorkerChoice: Codable, Equatable, Sendable {
    public let runtime: SubLiftRuntime
    public let engine: SubLiftEngine
    public let resolvedVia: ResolutionSource

    public init(runtime: SubLiftRuntime, engine: SubLiftEngine, resolvedVia: ResolutionSource) {
        self.runtime = runtime
        self.engine = engine
        self.resolvedVia = resolvedVia
    }
}

public enum RuntimePolicyError: Error, Equatable, LocalizedError {
    case unsupportedEngine(String)
    case pythonRuntimeRequested
    case invalidRuntime(String)
    case paddleCppUnavailable

    public var errorDescription: String? {
        switch self {
        case .unsupportedEngine(let e):
            let supported = SubLiftEngine.allCases.map { $0.rawValue }.sorted().joined(separator: ", ")
            return "Unsupported engine '\(e)'. Supported: [\(supported)]"
        case .pythonRuntimeRequested:
            return "SUBLIFT_RUNTIME=python is not a product option. Native is the only runtime. "
                + "Roll back to a previous product version instead."
        case .invalidRuntime(let r):
            return "SUBLIFT_RUNTIME='\(r)' is not a product option. Native is the only runtime."
        case .paddleCppUnavailable:
            return "Paddle C++ engine is unavailable on this system. Native capability is missing; "
                + "no Python fallback."
        }
    }
}

public enum RuntimePolicy {
    public static func resolve(
        requestedEngine: String = "vision",
        envOverride: [String: String]? = nil,
        isCppPaddleAvailable: Bool = false
    ) throws -> WorkerChoice {
        let engineNorm = requestedEngine.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        guard let engine = SubLiftEngine(rawValue: engineNorm) else {
            throw RuntimePolicyError.unsupportedEngine(requestedEngine)
        }

        let env = envOverride ?? ProcessInfo.processInfo.environment
        if let envVal = env["SUBLIFT_RUNTIME"] {
            let trimmed = envVal.trimmingCharacters(in: .whitespacesAndNewlines)
            if !trimmed.isEmpty {
                let lower = trimmed.lowercased()
                if lower == "python" {
                    throw RuntimePolicyError.pythonRuntimeRequested
                }
                if lower != "cpp" {
                    throw RuntimePolicyError.invalidRuntime(trimmed)
                }
            }
        }

        if engine == .paddle && !isCppPaddleAvailable {
            throw RuntimePolicyError.paddleCppUnavailable
        }

        return WorkerChoice(runtime: .cpp, engine: engine, resolvedVia: .productDefault)
    }
}
