import SwiftUI

/// 10104/10105：Context Inspector 容器。
///
/// - Video 模式：显示真实 metadata（10105）。
/// - Region/Extraction/Subtitle 模式：明确内部占位（对应 Feature 实现后填充），
///   不展示用户可见的假数据。
/// - 宽度固定 `WorkspaceLayout.inspectorReservedWidth`（300pt，280–360 合同内）；
///   用户关闭后由 WorkspaceModel 保持关闭（状态转换不强制重新打开）。
struct WorkspaceInspector: View {
    let mode: InspectorMode
    let metadataLoader: VideoMetadataLoader
    let playerModel: PlayerModel
    let videoURL: URL?
    let regionModel: RegionSelectionModel
    let extractor: SubtitleExtractor
    let workspaceState: WorkspaceState
    let editor: SubtitleEditor
    let transcriptAccessMode: TranscriptAccessMode
    let activeExtractionConfiguration: ExtractionConfiguration?
    let finalExtractionConfiguration: ExtractionConfiguration?
    let onRedetectRegion: () -> Void
    let onClose: () -> Void

    var body: some View {
        VStack(spacing: 0) {
            header
            Divider()
            content
        }
        .frame(width: WorkspaceLayout.inspectorReservedWidth)
        .frame(maxHeight: .infinity)
        .background(Color(nsColor: .windowBackgroundColor))
        .accessibilityElement(children: .contain)
        .accessibilityLabel("Inspector")
    }

    private var header: some View {
        HStack(spacing: 8) {
            Text(title)
                .font(.headline)
                .lineLimit(1)
            Spacer(minLength: 0)
            Button(action: onClose) {
                Image(systemName: "xmark.circle.fill")
                    .foregroundStyle(.secondary)
            }
            .buttonStyle(.borderless)
            .help("关闭 Inspector")
            .accessibilityLabel("关闭 Inspector")
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
    }

    @ViewBuilder
    private var content: some View {
        switch mode {
        case .video:
            if let videoURL {
                VideoInspector(
                    metadataLoader: metadataLoader,
                    playerModel: playerModel,
                    videoURL: videoURL
                )
            } else {
                placeholder
            }
        case .region:
            RegionInspector(
                regionModel: regionModel,
                onRedetectAtPlayhead: onRedetectRegion
            )
        case .extraction:
            ExtractionInspector(
                extractor: extractor,
                state: workspaceState,
                activeConfiguration: activeExtractionConfiguration,
                finalConfiguration: finalExtractionConfiguration
            )
        case .subtitle:
            SubtitleInspector(
                editor: editor,
                accessMode: transcriptAccessMode
            )
        }
    }

    private var placeholder: some View {
        VStack(spacing: 8) {
            Spacer(minLength: 0)
            Image(systemName: "sidebar.trailing")
                .font(.system(size: 28))
                .foregroundStyle(.tertiary)
            Text("详细内容将在 Inspector 升级中提供")
                .font(.callout)
                .foregroundStyle(.secondary)
                .multilineTextAlignment(.center)
            Spacer(minLength: 0)
        }
        .padding(16)
        .accessibilityElement(children: .combine)
    }

    private var title: String {
        switch mode {
        case .video: "视频信息"
        case .region: "字幕区域"
        case .extraction: "提取"
        case .subtitle: "字幕"
        }
    }
}
