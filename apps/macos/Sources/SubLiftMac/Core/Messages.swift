import Foundation

// MARK: - MessageType

/// IPC 消息类型常量，与 Python `sublift.ipc.protocol` 模块对齐。
public enum MessageType: String, Codable {
    // 控制消息
    case hello
    case bye
    case error

    // Swift → Python
    case startJob = "start_job"
    case frame
    case cancelJob = "cancel_job"

    // Python → Swift
    case progress
    case entries
    case log
    case done
}

// MARK: - Engine

/// OCR 引擎标识，与 Python `ENGINES` 集合对齐。
public enum OcrEngineName: String, Codable {
    case vision
    case paddle
}

// MARK: - LogLevel

/// 日志级别，与 Python `LOG_LEVELS` 集合对齐。
public enum LogLevel: String, Codable {
    case debug
    case info
    case warn
    case error
}

// MARK: - RegionBox

/// 字幕区域 [x, y, width, height]，与 Python `region_box` 对齐。
/// 用数组而非 struct，保持 JSON 传输紧凑。
public typealias RegionBox = [Int]

// MARK: - SubtitleEntryData

/// 字幕条目，与 Python `SubtitleEntry` 对齐。
/// 注意：`confidence` 是 feat-015 新增字段（Phase 1 SubtitleEntry 原无）。
public struct SubtitleEntryData: Codable, Equatable {
    public let startMs: Int
    public let endMs: Int
    public let text: String
    public let confidence: Double

    enum CodingKeys: String, CodingKey {
        case startMs = "start_ms"
        case endMs = "end_ms"
        case text
        case confidence
    }

    public init(startMs: Int, endMs: Int, text: String, confidence: Double) {
        self.startMs = startMs
        self.endMs = endMs
        self.text = text
        self.confidence = confidence
    }
}

// MARK: - Request Messages (Swift → Python)

/// 启动提取任务。
public struct StartJobMessage: Codable {
    public let type: MessageType
    public let videoId: String
    public let fps: Double
    public let engine: OcrEngineName
    public let confidenceThreshold: Double
    public let regionBox: RegionBox?

    enum CodingKeys: String, CodingKey {
        case type
        case videoId = "video_id"
        case fps
        case engine
        case confidenceThreshold = "confidence_threshold"
        case regionBox = "region_box"
    }

    public init(
        videoId: String,
        fps: Double,
        engine: OcrEngineName,
        confidenceThreshold: Double,
        regionBox: RegionBox? = nil
    ) {
        self.type = .startJob
        self.videoId = videoId
        self.fps = fps
        self.engine = engine
        self.confidenceThreshold = confidenceThreshold
        self.regionBox = regionBox
    }
}

/// 推送一帧 JPEG。
public struct FrameMessage: Codable {
    public let type: MessageType
    public let videoId: String
    public let tsMs: Int
    public let jpegBytes: String  // base64 encoded JPEG
    public let regionBox: RegionBox?

    enum CodingKeys: String, CodingKey {
        case type
        case videoId = "video_id"
        case tsMs = "ts_ms"
        case jpegBytes = "jpeg_bytes"
        case regionBox = "region_box"
    }

    public init(
        videoId: String,
        tsMs: Int,
        jpegBytes: String,
        regionBox: RegionBox? = nil
    ) {
        self.type = .frame
        self.videoId = videoId
        self.tsMs = tsMs
        self.jpegBytes = jpegBytes
        self.regionBox = regionBox
    }
}

/// 取消任务。
public struct CancelJobMessage: Codable {
    public let type: MessageType
    public let videoId: String

    enum CodingKeys: String, CodingKey {
        case type
        case videoId = "video_id"
    }

    public init(videoId: String) {
        self.type = .cancelJob
        self.videoId = videoId
    }
}

// MARK: - Response Messages (Python → Swift)

/// 进度通知。
public struct ProgressMessage: Codable {
    public let type: MessageType
    public let videoId: String
    public let stage: String
    public let pct: Double
    public let etaMs: Int

    enum CodingKeys: String, CodingKey {
        case type
        case videoId = "video_id"
        case stage
        case pct
        case etaMs = "eta_ms"
    }
}

/// 批量字幕条目。
public struct EntriesMessage: Codable {
    public let type: MessageType
    public let videoId: String
    public let entries: [SubtitleEntryData]

    enum CodingKeys: String, CodingKey {
        case type
        case videoId = "video_id"
        case entries
    }
}

/// 日志消息。
public struct LogMessage: Codable {
    public let type: MessageType
    public let videoId: String
    public let level: LogLevel
    public let msg: String

    enum CodingKeys: String, CodingKey {
        case type
        case videoId = "video_id"
        case level
        case msg
    }
}

/// 任务结束。
public struct DoneMessage: Codable {
    public let type: MessageType
    public let videoId: String
    public let ok: Bool
    public let error: String?

    enum CodingKeys: String, CodingKey {
        case type
        case videoId = "video_id"
        case ok
        case error
    }
}

// MARK: - Control Messages

/// 握手发起。
public struct HelloMessage: Codable {
    public let type: MessageType
    public let client: String

    public init(client: String = "sublift-mac") {
        self.type = .hello
        self.client = client
    }
}

/// 握手响应。
public struct ByeMessage: Codable {
    public let type: MessageType

    public init() {
        self.type = .bye
    }
}

/// 错误响应。
public struct ErrorMessage: Codable {
    public let type: MessageType
    public let message: String

    public init(message: String) {
        self.type = .error
        self.message = message
    }
}

// MARK: - Encoding Helpers

/// 消息编解码 helper。
/// feat-015 阶段提供 Codable 路径，与 PipelineClient 的 [String: Any] 路径并存。
public enum MessageCodec {
    /// 把 Codable 消息编码为 JSON Data。
    public static func encode<T: Encodable>(_ message: T) throws -> Data {
        try JSONEncoder().encode(message)
    }

    /// 从 JSON Data 解码指定类型的消息。
    public static func decode<T: Decodable>(_ data: Data, as type: T.Type) throws -> T {
        try JSONDecoder().decode(type, from: data)
    }
}
