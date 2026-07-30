import Foundation
import Darwin

/// Pipeline IPC 客户端：负责启动 Worker 子进程（C++ sublift_worker 或 Python server）并经 UDS 通信。
///
/// **并发约定**：每个实例由一次 extract 独占；`request*` 只在 slow 后台队列调用，
/// `stop()` 可从任意线程幂等调用（关闭 fd / terminate 进程 / unlink socket）。
public final class PipelineClient: @unchecked Sendable {
    /// 消息分帧：4 字节大端无符号整数表示 body 长度。
    static let lengthPrefixSize = 4
    /// 消息最大上限 (64 MiB)，对齐 Python/C++ 协议。
    static let maxMessageSize = 64 * 1024 * 1024

    /// Worker 子进程（C++ 或 Python）。
    private var process: Process?
    /// UDS socket 路径。
    private(set) var socketPath: String = ""
    /// 已连接的 socket 文件描述符。
    private var socketFD: Int32 = -1
    /// 最近一次成功握手的最终路由；供 GUI 显示真实 runtime / fallback 状态。
    public private(set) var lastWorkerChoice: WorkerChoice?

    public init() {}

    // MARK: - 消息编解码（纯函数，便于单测）

    /// 把消息字典打包成分帧字节流：4 字节大端长度前缀 + UTF-8 JSON body。
    public static func packMessage(_ message: [String: Any]) throws -> Data {
        let body = try JSONSerialization.data(
            withJSONObject: message,
            options: [.sortedKeys]
        )
        var data = Data(capacity: lengthPrefixSize + body.count)
        var length = UInt32(body.count).bigEndian
        withUnsafeBytes(of: &length) { ptr in
            data.append(contentsOf: ptr)
        }
        data.append(body)
        return data
    }

    /// 把 Codable 消息打包成分帧字节流（feat-015 新增）。
    public static func packMessage<T: Encodable>(_ message: T) throws -> Data {
        let body = try JSONEncoder().encode(message)
        var data = Data(capacity: lengthPrefixSize + body.count)
        var length = UInt32(body.count).bigEndian
        withUnsafeBytes(of: &length) { ptr in
            data.append(contentsOf: ptr)
        }
        data.append(body)
        return data
    }

    /// 从分帧字节流解析一条消息。
    /// - Parameter data: 完整的分帧字节（长度前缀 + body）
    /// - Returns: 解析后的消息字典；data 为空时返回 nil
    public static func unpackMessage(_ data: Data) throws -> [String: Any]? {
        guard !data.isEmpty else { return nil }
        guard data.count >= lengthPrefixSize else {
            throw PipelineClientError.incompleteLengthPrefix
        }

        let length = data.withUnsafeBytes { ptr -> UInt32 in
            ptr.load(as: UInt32.self).bigEndian
        }

        guard Int(length) <= maxMessageSize else {
            throw PipelineClientError.incompleteBody
        }

        let bodyStart = lengthPrefixSize
        let bodyEnd = lengthPrefixSize + Int(length)
        guard data.count >= bodyEnd else {
            throw PipelineClientError.incompleteBody
        }

        let bodyData = data.subdata(in: bodyStart..<bodyEnd)
        return try JSONSerialization.jsonObject(with: bodyData) as? [String: Any]
    }

    /// 从分帧字节流解析一条 Codable 消息（feat-015 新增）。
    public static func unpackMessage<T: Decodable>(
        _ data: Data,
        as type: T.Type
    ) throws -> T? {
        guard !data.isEmpty else { return nil }
        guard data.count >= lengthPrefixSize else {
            throw PipelineClientError.incompleteLengthPrefix
        }

        let length = data.withUnsafeBytes { ptr -> UInt32 in
            ptr.load(as: UInt32.self).bigEndian
        }

        guard Int(length) <= maxMessageSize else {
            throw PipelineClientError.incompleteBody
        }

        let bodyStart = lengthPrefixSize
        let bodyEnd = lengthPrefixSize + Int(length)
        guard data.count >= bodyEnd else {
            throw PipelineClientError.incompleteBody
        }

        let body = data.subdata(in: bodyStart..<bodyEnd)
        return try JSONDecoder().decode(type, from: body)
    }

    // MARK: - 可执行文件查找 (feat-06602)

    private static func isRegularExecutable(_ path: String, fileManager: FileManager) -> Bool {
        var isDir: ObjCBool = false
        guard fileManager.fileExists(atPath: path, isDirectory: &isDir), !isDir.boolValue else {
            return false
        }
        return fileManager.isExecutableFile(atPath: path)
    }

    /// Align with Python `probe_cpp_paddle_available`: ask the exact worker whether
    /// its compiled Paddle target and complete model set are product-usable.
    public static func probeCppPaddleAvailable(
        env: [String: String]? = nil,
        workerExecutable: String? = nil,
        fileManager: FileManager = .default
    ) -> Bool {
        let envMap = env ?? ProcessInfo.processInfo.environment
        let flag = (envMap["SUBLIFT_CPP_PADDLE"] ?? "")
            .trimmingCharacters(in: .whitespacesAndNewlines)
            .lowercased()
        if ["0", "false", "no", "off"].contains(flag) {
            return false
        }
        let workerPath: String
        if let workerExecutable {
            guard isRegularExecutable(workerExecutable, fileManager: fileManager) else {
                return false
            }
            workerPath = workerExecutable
        } else {
            guard let found = try? findWorkerExecutable(
                envOverride: envMap,
                fileManager: fileManager
            ) else {
                return false
            }
            workerPath = found
        }

        let probe = Process()
        probe.executableURL = URL(fileURLWithPath: workerPath)
        probe.arguments = ["--probe-engine", "paddle"]
        probe.environment = envMap
        probe.standardOutput = Pipe()
        probe.standardError = Pipe()
        do {
            try probe.run()
        } catch {
            return false
        }

        let deadline = Date().addingTimeInterval(2.0)
        while probe.isRunning && Date() < deadline {
            Thread.sleep(forTimeInterval: 0.01)
        }
        if probe.isRunning {
            probe.terminate()
            let terminateDeadline = Date().addingTimeInterval(0.5)
            while probe.isRunning && Date() < terminateDeadline {
                Thread.sleep(forTimeInterval: 0.01)
            }
            if probe.isRunning {
                kill(probe.processIdentifier, SIGKILL)
            }
            probe.waitUntilExit()
            return false
        }
        return probe.terminationStatus == 0
    }

    /// 查找 C++ sublift_worker 可执行文件路径。
    /// 查找顺序（与 Python `resolve_worker_bin` / 原生 CLI 对齐）：
    /// 1. 环境变量 SUBLIFT_WORKER_PATH
    /// 2. 仓库开发 build：`build/cpp-rel/bin`（Release）优先，再 `build/cpp/bin`（Debug）
    /// 3. App Bundle 资源目录 (Helpers/sublift_worker, Contents/MacOS/sublift_worker)
    /// 4. 系统 PATH 常用可执行路径 (/opt/homebrew/bin, /usr/local/bin, /usr/bin)
    public static func findWorkerExecutable(
        envOverride: [String: String]? = nil,
        fileManager: FileManager = .default
    ) throws -> String {
        var searchedPaths: [String] = []
        let env = envOverride ?? ProcessInfo.processInfo.environment

        // 1. 环境变量
        if let envPath = env["SUBLIFT_WORKER_PATH"], !envPath.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty {
            searchedPaths.append(envPath)
            if isRegularExecutable(envPath, fileManager: fileManager) {
                return envPath
            }
        }

        // 2. 仓库开发 build 目录（Release 优先，避免产品路径误用 Debug）
        if let repoRoot = findRepoRoot() {
            let candidates = [
                "build/cpp-rel/bin/sublift_worker",
                "build/cpp/bin/sublift_worker",
                "build/cpp-rel/sublift_worker",
                "build/cpp/sublift_worker",
            ]
            for rel in candidates {
                let path = repoRoot.appendingPathComponent(rel).path
                searchedPaths.append(path)
                if isRegularExecutable(path, fileManager: fileManager) {
                    return path
                }
            }
        }

        // 3. App Bundle 资源目录
        let mainBundle = Bundle.main
        let directHelperPath = mainBundle.bundlePath + "/Contents/Helpers/sublift_worker"
        searchedPaths.append(directHelperPath)
        if isRegularExecutable(directHelperPath, fileManager: fileManager) {
            return directHelperPath
        }

        if let resourceURL = mainBundle.resourceURL {
            let helperPath = resourceURL.appendingPathComponent("Helpers/sublift_worker").path
            searchedPaths.append(helperPath)
            if isRegularExecutable(helperPath, fileManager: fileManager) {
                return helperPath
            }
        }

        let macOsPath = mainBundle.bundlePath + "/Contents/MacOS/sublift_worker"
        searchedPaths.append(macOsPath)
        if isRegularExecutable(macOsPath, fileManager: fileManager) {
            return macOsPath
        }

        if let resPath = mainBundle.path(forResource: "sublift_worker", ofType: nil) {
            searchedPaths.append(resPath)
            if isRegularExecutable(resPath, fileManager: fileManager) {
                return resPath
            }
        }

        // 4. 系统 PATH
        let systemCandidates = [
            "/opt/homebrew/bin/sublift_worker",
            "/usr/local/bin/sublift_worker",
            "/usr/bin/sublift_worker"
        ]
        for sysPath in systemCandidates {
            searchedPaths.append(sysPath)
            if isRegularExecutable(sysPath, fileManager: fileManager) {
                return sysPath
            }
        }

        throw PipelineClientError.workerBinaryNotFound(searchedPaths: searchedPaths)
    }

    // MARK: - 子进程管理

    /// 启动 Worker 子进程（根据 RuntimePolicy 自动路由为 C++ sublift_worker 或 Python server）并连接 UDS。
    /// - Parameters:
    ///   - requestedRuntime: 显式强制 runtime ("python" | "cpp" | nil)
    ///   - engine: OCR 引擎（"vision"、"paddle" 或 "mock"，默认 "vision"）
    ///   - pythonExecutable: 自定义 Python 解释器路径（开发期默认 `.venv/bin/python`）
    ///   - workerExecutable: 自定义 C++ Worker 路径
    ///   - envOverride: 环境变量覆盖（用于单测）
    /// - Returns: 是否成功握手（发 hello 收 bye）
    @discardableResult
    public func start(
        requestedRuntime: String? = nil,
        engine: String = "vision",
        pythonExecutable: String? = nil,
        workerExecutable: String? = nil,
        envOverride: [String: String]? = nil
    ) throws -> Bool {
        // 先确保清理掉之前的进程和资源
        stop()
        lastWorkerChoice = nil

        let envForPolicy = envOverride ?? ProcessInfo.processInfo.environment
        let choice = try RuntimePolicy.resolve(
            requestedRuntime: requestedRuntime,
            requestedEngine: engine,
            envOverride: envForPolicy,
            isCppPaddleAvailable: Self.probeCppPaddleAvailable(
                env: envForPolicy,
                workerExecutable: workerExecutable
            )
        )

        let socketPath = makeSocketPath()
        self.socketPath = socketPath

        let process = Process()
        var env = envOverride ?? ProcessInfo.processInfo.environment

        // 继承环境并补上 Homebrew
        let extraPath = "/opt/homebrew/opt/ffmpeg-full/bin:/opt/homebrew/bin:/usr/local/bin"
        if let path = env["PATH"], !path.isEmpty {
            env["PATH"] = extraPath + ":" + path
        } else {
            env["PATH"] = extraPath + ":/usr/bin:/bin"
        }

        let repoRoot = Self.findRepoRoot()
        if let repoRoot {
            let srcPath = repoRoot.appendingPathComponent("src").path
            if let pp = env["PYTHONPATH"], !pp.isEmpty {
                env["PYTHONPATH"] = srcPath + ":" + pp
            } else {
                env["PYTHONPATH"] = srcPath
            }
            process.currentDirectoryURL = repoRoot
        }

        process.environment = env
        process.standardOutput = FileHandle.standardError
        process.standardError = FileHandle.standardError

        if choice.runtime == .cpp {
            let execPath = try (workerExecutable ?? Self.findWorkerExecutable(envOverride: env))
            process.executableURL = URL(fileURLWithPath: execPath)
            process.arguments = [
                "--socket", socketPath,
                "--engine", choice.engine.rawValue
            ]
            if choice.engine == .paddle {
                print("[SubLift] IPC worker start bin=\(execPath) engine=paddle runtime=cpp model=PP-OCRv6-small status=stable socket=\(socketPath) via=\(choice.resolvedVia.rawValue)")
            } else {
                print("[SubLift] IPC C++ worker start bin=\(execPath) engine=\(choice.engine.rawValue) socket=\(socketPath) via=\(choice.resolvedVia.rawValue)")
            }
        } else {
            let resolvedPath = pythonExecutable ?? Self.defaultPythonPath
            process.executableURL = URL(fileURLWithPath: resolvedPath)
            process.arguments = [
                "-m", "sublift.ipc.server",
                "--socket", socketPath,
                "--engine", choice.engine.rawValue,
                "--log-level", "INFO"
            ]
            if choice.engine == .paddle {
                let status = choice.runtime == .python ? "oracle_or_rollback" : "stable"
                print("[SubLift] IPC worker start python=\(resolvedPath) engine=paddle runtime=python model=PP-OCRv6-small status=\(status) socket=\(socketPath) via=\(choice.resolvedVia.rawValue)")
            } else {
                print("[SubLift] IPC Python server start python=\(resolvedPath) engine=\(choice.engine.rawValue) socket=\(socketPath) via=\(choice.resolvedVia.rawValue)")
            }
        }

        do {
            try process.run()
            self.process = process

            // 等待 socket 文件出现（最多 5 秒）
            try waitForSocket(at: socketPath, timeout: 5.0)

            // 连接 socket 并握手
            socketFD = try connectUDS(path: socketPath)
            let success = try handshake(requestedEngine: choice.engine.rawValue)
            if !success {
                stop()
            } else {
                lastWorkerChoice = choice
            }
            return success
        } catch {
            stop()
            throw error
        }
    }

    /// 停止子进程并清理资源。
    public func stop() {
        if socketFD >= 0 {
            close(socketFD)
            socketFD = -1
        }
        if let p = process {
            if p.isRunning {
                p.terminate()
                p.waitUntilExit()
            }
        }
        process = nil
        if !socketPath.isEmpty {
            unlink(socketPath)
            socketPath = ""
        }
    }

    // MARK: - 消息收发

    /// 发送一条字典消息并读取响应（兼容 hello/bye 等控制消息）。
    /// - Parameter message: 消息字典
    /// - Returns: 响应消息字典；连接关闭时返回 nil
    public func request(_ message: [String: Any]) throws -> [String: Any]? {
        let packed = try Self.packMessage(message)
        try writeAll(packed)
        return try readMessage()
    }

    /// 发送一条 Codable 消息并读取响应，解码为指定类型。
    /// - Parameters:
    ///   - message: 待发送的消息
    ///   - responseType: 期望的响应类型
    /// - Returns: 解码后的响应；连接关闭时返回 nil
    public func request<S: Encodable, R: Decodable>(
        _ message: S,
        expecting responseType: R.Type
    ) throws -> R? {
        let packed = try Self.packMessage(message)
        try writeAll(packed)
        guard let dict = try readMessageAny() else { return nil }
        try Self.throwIfServerError(dict)
        let bodyData = try JSONSerialization.data(withJSONObject: dict)
        return try JSONDecoder().decode(responseType, from: bodyData)
    }

    /// 发送一条 Codable 消息并读取响应，支持在主响应前接收增量 push_entry / progress。
    public func requestStreaming<S: Encodable, R: Decodable>(
        _ message: S,
        expecting responseType: R.Type,
        onPushEntry: ((SubtitleEntryData) -> Void)? = nil,
        onProgress: ((Double, String) -> Void)? = nil
    ) throws -> R {
        let packed = try Self.packMessage(message)
        try writeAll(packed)
        while true {
            guard let dict = try readMessageAny() else {
                throw PipelineClientError.connectionClosed
            }
            try Self.throwIfServerError(dict)
            let type = dict["type"] as? String
            if type == "push_entry" {
                let bodyData = try JSONSerialization.data(withJSONObject: dict)
                if let pushMsg = try? JSONDecoder().decode(PushEntryMessage.self, from: bodyData) {
                    onPushEntry?(pushMsg.entry)
                }
                continue
            }
            if type == "progress" {
                let pct = Self.jsonDouble(dict["pct"])
                let stage = dict["stage"] as? String ?? ""
                onProgress?(pct, stage)
                continue
            }
            if type == "log" {
                continue
            }
            if type == "done" {
                continue
            }
            let bodyData = try JSONSerialization.data(withJSONObject: dict)
            return try JSONDecoder().decode(responseType, from: bodyData)
        }
    }

    // MARK: - Private

    /// 校验握手并检查引擎匹配情况。
    private func handshake(requestedEngine: String) throws -> Bool {
        let helloMsg: [String: Any] = [
            "type": "hello",
            "client": "sublift-mac",
            "protocol_version": 1
        ]
        guard let bye = try request(helloMsg) else {
            return false
        }
        guard (bye["type"] as? String) == "bye" else {
            return false
        }

        if let engines = bye["engines"] as? [String] {
            if !engines.contains(requestedEngine) {
                stop()
                throw PipelineClientError.engineMismatch(requested: requestedEngine, supported: engines)
            }
        }
        return true
    }

    /// 将 Worker 端的业务失败统一映射为可本地化展示的 serverError。
    private static func throwIfServerError(_ dict: [String: Any]) throws {
        let type = dict["type"] as? String
        if type == "done", (dict["ok"] as? Bool ?? false) == false {
            let error = dict["error"] as? String ?? "unknown error"
            throw PipelineClientError.serverError(error)
        }
        if type == "error" {
            let error = dict["message"] as? String ?? "protocol error"
            throw PipelineClientError.serverError(error)
        }
    }

    /// JSON 数字（常为 NSNumber）→ Double。
    private static func jsonDouble(_ value: Any?) -> Double {
        if let d = value as? Double { return d }
        if let i = value as? Int { return Double(i) }
        if let n = value as? NSNumber { return n.doubleValue }
        if let s = value as? String, let d = Double(s) { return d }
        return 0
    }

    /// 向上查找含 `.venv` 与 `src/sublift` 的仓库根。
    static func findRepoRoot() -> URL? {
        var currentURL = URL(fileURLWithPath: Bundle.main.bundlePath)
        for _ in 0..<8 {
            let venv = currentURL.appendingPathComponent(".venv/bin/python")
            let pkg = currentURL.appendingPathComponent("src/sublift")
            if FileManager.default.fileExists(atPath: venv.path),
               FileManager.default.fileExists(atPath: pkg.path) {
                return currentURL
            }
            currentURL = currentURL.deletingLastPathComponent()
        }

        var cwd = URL(fileURLWithPath: FileManager.default.currentDirectoryPath)
        for _ in 0..<6 {
            let venv = cwd.appendingPathComponent(".venv/bin/python")
            let pkg = cwd.appendingPathComponent("src/sublift")
            if FileManager.default.fileExists(atPath: venv.path),
               FileManager.default.fileExists(atPath: pkg.path) {
                return cwd
            }
            cwd = cwd.deletingLastPathComponent()
        }
        return nil
    }

    /// 开发期默认 Python 路径（项目根 `.venv/bin/python`）。
    static let defaultPythonPath: String = {
        if let root = findRepoRoot() {
            return root.appendingPathComponent(".venv/bin/python").path
        }
        return Bundle.main.bundlePath + "/../../.venv/bin/python"
    }()

    private func makeSocketPath() -> String {
        let tempDir = NSTemporaryDirectory()
        return (tempDir as NSString).appendingPathComponent(
            "sublift-\(UUID().uuidString.prefix(8)).sock"
        )
    }

    private func waitForSocket(at path: String, timeout: TimeInterval) throws {
        let deadline = Date().addingTimeInterval(timeout)
        while Date() < deadline {
            if FileManager.default.fileExists(atPath: path) { return }
            Thread.sleep(forTimeInterval: 0.01)
        }
        throw PipelineClientError.serverStartTimeout
    }

    private func connectUDS(path: String) throws -> Int32 {
        let fd = socket(AF_UNIX, SOCK_STREAM, 0)
        guard fd >= 0 else {
            throw PipelineClientError.socketCreateFailed(errno)
        }

        // 防护 SIGPIPE：当对端进程退出时，向 Socket 发送数据不触发 SIGPIPE 崩溃 GUI 进程
        var nosigpipe: Int32 = 1
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &nosigpipe, socklen_t(MemoryLayout<Int32>.size))

        var addr = sockaddr_un()
        addr.sun_family = sa_family_t(AF_UNIX)
        let pathBytes = path.utf8CString
        withUnsafeMutablePointer(to: &addr.sun_path) { ptr in
            ptr.withMemoryRebound(to: CChar.self, capacity: pathBytes.count) { dest in
                pathBytes.withUnsafeBufferPointer { src in
                    _ = memcpy(dest, src.baseAddress, src.count)
                }
            }
        }

        let res = withUnsafePointer(to: &addr) { ptr in
            ptr.withMemoryRebound(to: sockaddr.self, capacity: 1) { dest in
                connect(fd, dest, socklen_t(MemoryLayout<sockaddr_un>.size))
            }
        }

        guard res == 0 else {
            close(fd)
            throw PipelineClientError.socketConnectFailed(errno)
        }

        return fd
    }

    private func readMessage() throws -> [String: Any]? {
        var lengthBytes = [UInt8](repeating: 0, count: Self.lengthPrefixSize)
        let readResult = lengthBytes.withUnsafeMutableBufferPointer { ptr in
            recv(socketFD, ptr.baseAddress, ptr.count, Int32(MSG_WAITALL))
        }
        guard readResult == Self.lengthPrefixSize else { return nil }

        let length = UInt32(lengthBytes[0]) << 24
            | UInt32(lengthBytes[1]) << 16
            | UInt32(lengthBytes[2]) << 8
            | UInt32(lengthBytes[3])

        guard Int(length) <= Self.maxMessageSize else {
            throw PipelineClientError.incompleteBody
        }

        var bodyBytes = [UInt8](repeating: 0, count: Int(length))
        let bodyResult = bodyBytes.withUnsafeMutableBufferPointer { ptr in
            recv(socketFD, ptr.baseAddress, ptr.count, Int32(MSG_WAITALL))
        }
        guard bodyResult == Int(length) else { return nil }

        let bodyData = Data(bodyBytes)
        return try JSONSerialization.jsonObject(with: bodyData) as? [String: Any]
    }

    private func readMessageAny() throws -> [String: Any]? {
        return try readMessage()
    }

    private func writeAll(_ data: Data) throws {
        try data.withUnsafeBytes { (ptr: UnsafeRawBufferPointer) in
            var sent = 0
            while sent < data.count {
                let n = send(
                    socketFD,
                    ptr.baseAddress?.advanced(by: sent),
                    data.count - sent,
                    0
                )
                guard n > 0 else {
                    throw PipelineClientError.socketWriteFailed(errno)
                }
                sent += n
            }
        }
    }
}

// MARK: - Errors

public enum PipelineClientError: Error, Equatable {
    case incompleteLengthPrefix
    case incompleteBody
    case serverStartTimeout
    case socketCreateFailed(Int32)
    case socketConnectFailed(Int32)
    case socketWriteFailed(Int32)
    /// Python 或 C++ Worker 返回 `done(ok=false)` 或 `error`。
    case serverError(String)
    /// 对端关闭连接（含用户取消关闭 socket）。
    case connectionClosed
    /// 未找到 C++ Worker 可执行文件。
    case workerBinaryNotFound(searchedPaths: [String])
    /// 请求的引擎与 Worker 声明的能力不匹配。
    case engineMismatch(requested: String, supported: [String])
}

extension PipelineClientError: LocalizedError {
    public var errorDescription: String? {
        switch self {
        case .serverError(let message):
            return message
        case .incompleteLengthPrefix:
            return "与提取服务通信失败：长度前缀不完整。"
        case .incompleteBody:
            return "与提取服务通信失败：消息体不完整。"
        case .serverStartTimeout:
            return "提取服务启动超时。"
        case .socketCreateFailed(let errno):
            return "创建 socket 失败（errno \(errno)）。"
        case .socketConnectFailed(let errno):
            return "连接提取服务失败（errno \(errno)）。"
        case .socketWriteFailed(let errno):
            return "向提取服务写入失败（errno \(errno)）。"
        case .connectionClosed:
            return "与提取服务的连接已关闭。"
        case .workerBinaryNotFound(let searchedPaths):
            return "未找到 SubLift C++ Worker 可执行文件（已检索路径: \(searchedPaths.joined(separator: ", "))）。"
        case .engineMismatch(let requested, let supported):
            return "请求的引擎 '\(requested)' 不被当前 Worker 支持（支持的引擎: [\(supported.joined(separator: ", "))]）。"
        }
    }
}
