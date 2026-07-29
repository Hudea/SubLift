import Foundation

public enum SubLiftRuntime: String, Codable, CaseIterable, Equatable, Sendable {
    case python = "python"
    case cpp = "cpp"
}

public enum SubLiftEngine: String, Codable, CaseIterable, Equatable, Sendable {
    case vision = "vision"
    case mock = "mock"
    case paddle = "paddle"
}

public enum ResolutionSource: String, Codable, Equatable, Sendable {
    case explicitFlag = "explicit_flag"
    case envVar = "env_var"
    case productDefault = "product_default"
    case paddleOverride = "paddle_override"
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
    case invalidRuntime(String)

    public var errorDescription: String? {
        switch self {
        case .unsupportedEngine(let e):
            let supported = SubLiftEngine.allCases.map { $0.rawValue }.sorted().joined(separator: ", ")
            return "Unsupported engine '\(e)'. Supported: [\(supported)]"
        case .invalidRuntime(let r):
            let supported = SubLiftRuntime.allCases.map { $0.rawValue }.sorted().joined(separator: ", ")
            return "Invalid runtime '\(r)'. Supported: [\(supported)]"
        }
    }
}

public enum RuntimePolicy {
    public static func resolve(
        requestedRuntime: String? = nil,
        requestedEngine: String = "vision",
        envOverride: [String: String]? = nil,
        defaultRuntime: SubLiftRuntime = .cpp,
        isCppPaddleAvailable: Bool = false
    ) throws -> WorkerChoice {
        let engineNorm = requestedEngine.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        guard let engine = SubLiftEngine(rawValue: engineNorm) else {
            throw RuntimePolicyError.unsupportedEngine(requestedEngine)
        }

        var source: ResolutionSource = .productDefault
        var candidateRuntimeStr: String = defaultRuntime.rawValue

        if let flag = requestedRuntime, !flag.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            candidateRuntimeStr = flag.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
            source = .explicitFlag
        } else {
            let env = envOverride ?? ProcessInfo.processInfo.environment
            if let envVal = env["SUBLIFT_RUNTIME"], !envVal.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
                candidateRuntimeStr = envVal.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
                source = .envVar
            }
        }

        guard let candidateRuntime = SubLiftRuntime(rawValue: candidateRuntimeStr) else {
            throw RuntimePolicyError.invalidRuntime(candidateRuntimeStr)
        }

        if engine == .paddle {
            if candidateRuntime == .cpp {
                if isCppPaddleAvailable {
                    return WorkerChoice(runtime: .cpp, engine: .paddle, resolvedVia: source)
                }
                return WorkerChoice(runtime: .python, engine: .paddle, resolvedVia: .paddleOverride)
            }
            return WorkerChoice(runtime: .python, engine: .paddle, resolvedVia: source)
        }

        return WorkerChoice(runtime: candidateRuntime, engine: engine, resolvedVia: source)
    }
}
