import Foundation

/// 08511：队列级输出目的地（显式 Codable，禁止依赖 Swift 合成枚举 JSON）。
enum BatchOutputDestination: Equatable, Sendable {
    case sidecar
    case publicRoot(URL)
}

extension BatchOutputDestination: Codable {
    private enum DestinationCodingKeys: String, CodingKey {
        case type
        case url
    }

    func encode(to encoder: Encoder) throws {
        var c = encoder.container(keyedBy: DestinationCodingKeys.self)
        switch self {
        case .sidecar:
            try c.encode("sidecar", forKey: .type)
        case .publicRoot(let url):
            try c.encode("publicRoot", forKey: .type)
            try c.encode(url, forKey: .url)
        }
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: DestinationCodingKeys.self)
        let type = try c.decode(String.self, forKey: .type)
        switch type {
        case "sidecar":
            self = .sidecar
        case "publicRoot":
            let url = try c.decode(URL.self, forKey: .url)
            self = .publicRoot(url)
        default:
            throw DecodingError.dataCorruptedError(forKey: .type, in: c, debugDescription: "未知的 outputDestination 类型: \(type)")
        }
    }
}
