import Foundation

/// Pipeline IPC 客户端：负责启动 Python 子进程并经 UDS 通信。
///
/// feat-014：启动 Python 子进程 + UDS 连接 + hello/bye 握手。
/// feat-015：新增 Codable 消息编解码重载（packMessage/unpackMessage 泛型版本）。
/// feat-016：将接入 Pipeline 帧流。
///
/// **并发约定**：每个实例由一次 extract 独占；`request*` 只在单一后台队列调用，
/// `stop()` 可从任意线程幂等调用（关闭 fd / terminate 进程）。跨队列传递依赖此约定。
public final class PipelineClient: @unchecked Sendable {
    /// 消息分帧：4 字节大端无符号整数表示 body 长度。
    static let lengthPrefixSize = 4

    /// Python 子进程。
    private var process: Process?
    /// UDS socket 路径。
    private(set) var socketPath: String = ""
    /// 已连接的 socket 文件描述符。
    private var socketFD: Int32 = -1

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

        let length = data.subdata(in: 0..<lengthPrefixSize).withUnsafeBytes { ptr in
            ptr.load(as: UInt32.self).bigEndian
        }

        let bodyStart = lengthPrefixSize
        let bodyEnd = bodyStart + Int(length)
        guard data.count >= bodyEnd else {
            throw PipelineClientError.incompleteBody
        }

        let body = data.subdata(in: bodyStart..<bodyEnd)
        return try JSONSerialization.jsonObject(with: body) as? [String: Any]
    }

    /// 从分帧字节流解析一条 Codable 消息（feat-015 新增）。
    /// - Parameters:
    ///   - data: 完整的分帧字节（长度前缀 + body）
    ///   - type: 目标 Codable 类型
    /// - Returns: 解析后的消息；data 为空时返回 nil
    public static func unpackMessage<T: Decodable>(
        _ data: Data,
        as type: T.Type
    ) throws -> T? {
        guard !data.isEmpty else { return nil }
        guard data.count >= lengthPrefixSize else {
            throw PipelineClientError.incompleteLengthPrefix
        }

        let length = data.subdata(in: 0..<lengthPrefixSize).withUnsafeBytes { ptr in
            ptr.load(as: UInt32.self).bigEndian
        }

        let bodyStart = lengthPrefixSize
        let bodyEnd = bodyStart + Int(length)
        guard data.count >= bodyEnd else {
            throw PipelineClientError.incompleteBody
        }

        let body = data.subdata(in: bodyStart..<bodyEnd)
        return try JSONDecoder().decode(type, from: body)
    }

    // MARK: - 子进程管理

    /// 启动 Python 子进程并连接 UDS。
    /// - Parameters:
    ///   - pythonExecutable: Python 解释器路径（开发期默认 `.venv/bin/python`）
    ///   - engine: OCR 引擎（"vision"、"paddle" 或 "mock"，默认 "vision"）
    /// - Returns: 是否成功握手（发 hello 收 bye）
    @discardableResult
    public func start(
        pythonExecutable: String? = nil,
        engine: String = "vision"
    ) throws -> Bool {
        let resolvedPath = pythonExecutable ?? Self.defaultPythonPath
        let socketPath = makeSocketPath()
        self.socketPath = socketPath

        let process = Process()
        process.executableURL = URL(fileURLWithPath: resolvedPath)
        process.arguments = [
            "-m", "sublift.ipc.server",
            "--socket", socketPath,
            "--engine", engine,
            "--log-level", "INFO",
        ]
        // 继承环境并补上 Homebrew，避免 GUI 子进程找不到 ffmpeg
        var env = ProcessInfo.processInfo.environment
        let extraPath = "/opt/homebrew/opt/ffmpeg-full/bin:/opt/homebrew/bin:/usr/local/bin"
        if let path = env["PATH"], !path.isEmpty {
            env["PATH"] = extraPath + ":" + path
        } else {
            env["PATH"] = extraPath + ":/usr/bin:/bin"
        }
        // 保证能 import 可编辑安装的 sublift（从任意 cwd 启动）
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
        // 把 Python stderr/stdout 接到当前进程，终端才能看到诊断日志
        process.standardOutput = FileHandle.standardError
        process.standardError = FileHandle.standardError
        print(
            "[SubLift] IPC server start python=\(resolvedPath) engine=\(engine) socket=\(socketPath) cwd=\(process.currentDirectoryURL?.path ?? "?")"
        )
        try process.run()
        self.process = process

        // 等待 socket 文件出现（最多 2 秒）
        try waitForSocket(at: socketPath, timeout: 2.0)

        // 连接 socket 并握手
        socketFD = try connectUDS(path: socketPath)
        return try handshake()
    }

    /// 停止子进程并清理资源。
    public func stop() {
        if socketFD >= 0 {
            close(socketFD)
            socketFD = -1
        }
        process?.terminate()
        process = nil
        if !socketPath.isEmpty {
            unlink(socketPath)
            socketPath = ""
        }
    }

    // MARK: - 消息收发（feat-015 暴露，feat-016 bridge 会使用）

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
    ///
    /// 跳过 `progress` / `log`；`push_entry` 交给回调；`done(ok=false)` / `error` 抛错；
    /// 其余类型按 `responseType` 解码为主响应（path mode 下多为 `entries`）。
    ///
    /// - Parameters:
    ///   - message: 待发送的消息
    ///   - responseType: 期望的主响应类型
    ///   - onPushEntry: 增量 push_entry 回调
    ///   - onProgress: 可选 progress 回调（pct 0...1）
    /// - Returns: 解码后的主响应
    /// - Throws: `connectionClosed` 若对端关闭（含用户取消关 socket）
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
                // JSONSerialization 数字多为 NSNumber，as? Double 会失败导致进度永远 0
                let pct = Self.jsonDouble(dict["pct"])
                let stage = dict["stage"] as? String ?? ""
                onProgress?(pct, stage)
                continue
            }
            if type == "log" {
                continue
            }
            if type == "done" {
                // path mode 失败用 done；成功主响应是 entries，忽略 ok=true 的 done
                continue
            }
            let bodyData = try JSONSerialization.data(withJSONObject: dict)
            return try JSONDecoder().decode(responseType, from: bodyData)
        }
    }

    // MARK: - Private

    /// 将 Python 端的业务失败统一映射为可本地化展示的 serverError。
    /// 非流式 legacy frame mode 与流式 path mode 都复用此分支。
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
        // 再从 cwd 试
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
    /// TODO(feat-025): 打包时改为 embedded Python.framework 路径。
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

        let result = withUnsafePointer(to: &addr) { addrPtr in
            addrPtr.withMemoryRebound(to: sockaddr.self, capacity: 1) { sockaddrPtr in
                connect(fd, sockaddrPtr, socklen_t(MemoryLayout<sockaddr_un>.size))
            }
        }
        guard result == 0 else {
            close(fd)
            throw PipelineClientError.socketConnectFailed(errno)
        }
        return fd
    }

    /// 握手：发 hello 收 bye。
    private func handshake() throws -> Bool {
        let hello: [String: Any] = ["type": "hello", "client": "sublift-mac"]
        let packed = try Self.packMessage(hello)
        try writeAll(packed)

        guard let response = try readMessage() else {
            return false
        }
        return (response["type"] as? String) == "bye"
    }

    /// 从 socket 读取一条分帧消息。
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

        var bodyBytes = [UInt8](repeating: 0, count: Int(length))
        let bodyResult = bodyBytes.withUnsafeMutableBufferPointer { ptr in
            recv(socketFD, ptr.baseAddress, ptr.count, Int32(MSG_WAITALL))
        }
        guard bodyResult == Int(length) else { return nil }

        let bodyData = Data(bodyBytes)
        return try JSONSerialization.jsonObject(with: bodyData) as? [String: Any]
    }

    /// 从 socket 读取一条分帧消息并解码为指定类型。
    private func readMessageDecoded<T: Decodable>(as type: T.Type) throws -> T? {
        var lengthBytes = [UInt8](repeating: 0, count: Self.lengthPrefixSize)
        let readResult = lengthBytes.withUnsafeMutableBufferPointer { ptr in
            recv(socketFD, ptr.baseAddress, ptr.count, Int32(MSG_WAITALL))
        }
        guard readResult == Self.lengthPrefixSize else { return nil }

        let length = UInt32(lengthBytes[0]) << 24
            | UInt32(lengthBytes[1]) << 16
            | UInt32(lengthBytes[2]) << 8
            | UInt32(lengthBytes[3])

        var bodyBytes = [UInt8](repeating: 0, count: Int(length))
        let bodyResult = bodyBytes.withUnsafeMutableBufferPointer { ptr in
            recv(socketFD, ptr.baseAddress, ptr.count, Int32(MSG_WAITALL))
        }
        guard bodyResult == Int(length) else { return nil }

        let bodyData = Data(bodyBytes)
        return try JSONDecoder().decode(type, from: bodyData)
    }

    /// 从 socket 读取一条分帧消息，返回原始字典（用于流式模式判断类型）。
    private func readMessageAny() throws -> [String: Any]? {
        var lengthBytes = [UInt8](repeating: 0, count: Self.lengthPrefixSize)
        let readResult = lengthBytes.withUnsafeMutableBufferPointer { ptr in
            recv(socketFD, ptr.baseAddress, ptr.count, Int32(MSG_WAITALL))
        }
        guard readResult == Self.lengthPrefixSize else { return nil }

        let length = UInt32(lengthBytes[0]) << 24
            | UInt32(lengthBytes[1]) << 16
            | UInt32(lengthBytes[2]) << 8
            | UInt32(lengthBytes[3])

        var bodyBytes = [UInt8](repeating: 0, count: Int(length))
        let bodyResult = bodyBytes.withUnsafeMutableBufferPointer { ptr in
            recv(socketFD, ptr.baseAddress, ptr.count, Int32(MSG_WAITALL))
        }
        guard bodyResult == Int(length) else { return nil }

        let bodyData = Data(bodyBytes)
        return try JSONSerialization.jsonObject(with: bodyData) as? [String: Any]
    }

    /// 把 Data 全部写入 socket。
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
    /// Python 返回 `done(ok=false)` 或 `error`。
    case serverError(String)
    /// 对端关闭连接（含用户取消关闭 socket）。
    case connectionClosed
}

extension PipelineClientError: LocalizedError {
    /// 暴露可操作的错误文案，避免 `error.localizedDescription` 退化为系统默认文案。
    /// 特别是 `serverError` 携带的 Python 端消息（如 `uv sync --extra paddle`、
    /// 网络或缓存错误）需要透传给用户。
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
        }
    }
}
