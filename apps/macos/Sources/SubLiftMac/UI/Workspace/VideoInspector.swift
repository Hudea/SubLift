import SwiftUI

/// 10105：Video Inspector 展示层纯逻辑（可单测）。
///
/// 只展示真实字段：文件名、分辨率、时长、编码、大小、容器、预览兼容性；
/// 容器名与预览兼容性为可推导事实，不编造数据。
enum VideoInspectorPresentation {

    struct Row: Equatable {
        let label: String
        let value: String
        let icon: String
    }

    /// 容器名（按扩展名映射；未知扩展名原样大写，不编造）。
    static func containerName(for url: URL) -> String {
        switch url.pathExtension.lowercased() {
        case "mp4": return "MPEG-4 (MP4)"
        case "mov": return "QuickTime (MOV)"
        case "mkv": return "Matroska (MKV)"
        default: return url.pathExtension.uppercased()
        }
    }

    /// 预览兼容性：AVPlayer 失败（如 mkv）时由 ffmpeg 静态预览。
    static func previewCompatibility(loadFailed: Bool) -> String {
        loadFailed
            ? "静态预览（AVPlayer 不支持，使用 ffmpeg 抽帧）"
            : "可正常播放"
    }

    /// 组装展示行：全部来自真实 metadata 或可推导事实。
    static func rows(metadata: VideoMetadata, url: URL, loadFailed: Bool) -> [Row] {
        [
            Row(label: "文件名", value: metadata.fileName, icon: "film"),
            Row(label: "分辨率", value: VideoMetadata.formatResolution(width: metadata.width, height: metadata.height), icon: "aspectratio"),
            Row(label: "时长", value: TimeFormatter.formatMs(metadata.durationMs), icon: "clock"),
            Row(label: "编码", value: metadata.codec, icon: "cpu"),
            Row(label: "大小", value: VideoMetadata.formatFileSize(metadata.fileSize), icon: "externaldrive"),
            Row(label: "容器", value: containerName(for: url), icon: "shippingbox"),
            Row(label: "预览", value: previewCompatibility(loadFailed: loadFailed), icon: loadFailed ? "exclamationmark.triangle" : "play.circle"),
        ]
    }
}

/// 10105：Video 模式 Inspector 内容。
struct VideoInspector: View {
    @ObservedObject var metadataLoader: VideoMetadataLoader
    @ObservedObject var playerModel: PlayerModel
    let videoURL: URL

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            if let metadata = metadataLoader.metadata {
                rowsList(metadata: metadata)
            } else {
                HStack(spacing: 8) {
                    ProgressView()
                        .controlSize(.small)
                    Text("正在解析视频信息…")
                        .font(.callout)
                        .foregroundStyle(.secondary)
                }
                .padding(12)
            }
            Spacer(minLength: 0)
        }
        .accessibilityElement(children: .contain)
    }

    private func rowsList(metadata: VideoMetadata) -> some View {
        let rows = VideoInspectorPresentation.rows(
            metadata: metadata,
            url: videoURL,
            loadFailed: playerModel.loadFailed
        )
        return VStack(alignment: .leading, spacing: 0) {
            ForEach(Array(rows.enumerated()), id: \.offset) { _, row in
                HStack(alignment: .firstTextBaseline, spacing: 10) {
                    Image(systemName: row.icon)
                        .font(.system(size: 12))
                        .foregroundStyle(.secondary)
                        .frame(width: 16)
                    Text(row.label)
                        .font(.callout)
                        .foregroundStyle(.secondary)
                    Spacer(minLength: 4)
                    Text(row.value)
                        .font(.system(.callout, design: .monospaced))
                        .lineLimit(2)
                        .multilineTextAlignment(.trailing)
                }
                .padding(.horizontal, 12)
                .padding(.vertical, 7)
                .accessibilityElement(children: .combine)
                .accessibilityLabel("\(row.label)：\(row.value)")

                if row.label != "预览" {
                    Divider()
                        .padding(.leading, 38)
                }
            }
        }
    }
}
