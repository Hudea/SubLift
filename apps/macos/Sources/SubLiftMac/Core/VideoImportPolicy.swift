import Foundation
import UniformTypeIdentifiers

/// 受支持的视频导入格式（10103 合同：MP4 / MOV / MKV）。
enum ImportFormat: String, Equatable, Hashable, Sendable {
    case mp4
    case mov
    case mkv
}

/// 导入校验结果：View 层拖拽反馈与 WorkspaceModel 导入门共用。
enum VideoImportValidation: Equatable, Sendable {
    case valid(format: ImportFormat)
    case unsupportedFormat(pathExtension: String)
    case mkvRequiresFfmpeg
}

/// 视频导入策略：格式白名单与 ffmpeg 依赖的纯逻辑真源。
///
/// - 不依赖 SwiftUI；`ffmpegAvailable` 可注入以便确定性测试。
/// - Open Panel 与 drop 共用同一校验，保证两入口行为一致。
struct VideoImportPolicy {

    /// 受支持扩展名（小写）。
    static let supportedExtensions: Set<String> = ["mp4", "mov", "mkv"]

    /// Welcome 中展示的格式提示文案。
    static let supportedFormatsText = "MP4 · MOV · MKV"

    /// Open Panel 使用的 UTType 列表（mp4/mov/mkv）。
    static var openPanelContentTypes: [UTType] {
        var types: [UTType] = [.mpeg4Movie, .quickTimeMovie]
        if let mkv = UTType(filenameExtension: "mkv") {
            types.append(mkv)
        }
        return types
    }

    /// ffmpeg 可用性探测（默认走系统检测，测试可注入）。
    var ffmpegAvailable: () -> Bool

    init(ffmpegAvailable: @escaping () -> Bool = { FfmpegDetector.detect() != nil }) {
        self.ffmpegAvailable = ffmpegAvailable
    }

    /// 校验导入 URL。MKV 在 ffmpeg 缺失时返回 `.mkvRequiresFfmpeg`（fail-closed）。
    func validate(url: URL) -> VideoImportValidation {
        let ext = url.pathExtension.lowercased()
        guard let format = ImportFormat(rawValue: ext), Self.supportedExtensions.contains(ext) else {
            return .unsupportedFormat(pathExtension: ext)
        }
        if format == .mkv && !ffmpegAvailable() {
            return .mkvRequiresFfmpeg
        }
        return .valid(format: format)
    }
}
