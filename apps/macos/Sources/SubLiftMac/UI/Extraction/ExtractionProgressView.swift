import SwiftUI

// MARK: - 展示纯逻辑（10208）

/// 提取进度展示：只投影真实 status 字段；不显示不可可靠推导的 ETA/平均置信度。
enum ExtractionProgressPresentation {

    /// "已处理/总帧（百分比）"——全部来自真实 status。
    static func progressText(pct: Double, frameCount: Int, totalFrames: Int) -> String {
        guard totalFrames > 0, pct.isFinite else {
            return frameCount > 0 ? "已处理 \(frameCount) 帧" : "处理中…"
        }
        return "\(frameCount)/\(totalFrames)（\(Int(pct * 100))%）"
    }

    /// 实时倍速（ProcessingRate 有值才显示）。
    static func rateText(_ rate: Double?) -> String? {
        ProcessingRate.displayText(for: rate)
    }

    /// 实际 runtime 身份（真实 extractor 值）。
    static func runtimeText(identity: String?) -> String? {
        identity.map { "OCR 运行时：\($0)" }
    }

    /// 阶段文案（真实状态映射；idle 无）。
    static func stageText(_ status: SubtitleExtractor.Status?) -> String? {
        switch status {
        case .startingServer: "正在启动服务…"
        case .finalizing: "正在整理字幕…"
        case .done(let entryCount): "完成，识别 \(entryCount) 条"
        case .error(let message): message
        default: nil
        }
    }
}

/// Live Transcript 只读说明（processing/finalizing 时显示，VoiceOver 可读）。
enum TranscriptReadOnlyNotice {
    static func text(for state: WorkspaceState) -> String? {
        switch state {
        case .starting, .processing, .finalizing:
            "识别结果生成中，只读"
        default:
            nil
        }
    }
}

// MARK: - ExtractionProgressView

/// 10208：紧凑提取进度区（视频工作区底部）。
///
/// 只显示真实 pct/frame/rate/runtime；Stop 主动作在 Toolbar。
struct ExtractionProgressView: View {
    @ObservedObject var extractor: SubtitleExtractor

    var body: some View {
        VStack(spacing: 6) {
            HStack(spacing: 8) {
                Image(systemName: statusIcon)
                    .foregroundStyle(statusColor)
                    .accessibilityHidden(true)
                Text(statusText)
                    .font(.system(.callout, design: .monospaced))
                    .foregroundStyle(statusColor)
                    .lineLimit(1)
                    .truncationMode(.tail)
                Spacer(minLength: 0)
                if let rateText = ExtractionProgressPresentation.rateText(extractor.processingRate) {
                    Text(rateText)
                        .font(.system(.callout, design: .monospaced))
                        .foregroundStyle(.secondary)
                }
            }
            .accessibilityElement(children: .combine)

            if let runtimeText = ExtractionProgressPresentation.runtimeText(identity: extractor.runtimeIdentity) {
                Text(runtimeText)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .accessibilityLabel(runtimeText)
            }

            progressBar
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
    }

    @ViewBuilder
    private var progressBar: some View {
        switch extractor.status {
        case .processing(let progress, let frameCount, let totalFrames):
            ProgressView(value: progress)
                .progressViewStyle(.linear)
                .accessibilityLabel("提取进度")
                .accessibilityValue(ExtractionProgressPresentation.progressText(
                    pct: progress,
                    frameCount: frameCount,
                    totalFrames: totalFrames
                ))
        case .finalizing:
            ProgressView()
                .progressViewStyle(.linear)
                .accessibilityLabel("正在整理字幕")
        default:
            EmptyView()
        }
    }

    private var statusText: String {
        switch extractor.status {
        case .idle:
            return "就绪"
        case .startingServer:
            return ExtractionProgressPresentation.stageText(.startingServer) ?? "启动中…"
        case .processing(let pct, let frameCount, let totalFrames):
            return ExtractionProgressPresentation.progressText(pct: pct, frameCount: frameCount, totalFrames: totalFrames)
        case .finalizing:
            return ExtractionProgressPresentation.stageText(.finalizing) ?? "整理中…"
        case .done(let count):
            return ExtractionProgressPresentation.stageText(.done(entryCount: count)) ?? "完成"
        case .error(let message):
            return message
        }
    }

    private var statusIcon: String {
        switch extractor.status {
        case .processing, .startingServer: "waveform.path.ecg"
        case .finalizing: "checklist"
        case .done: "checkmark.circle"
        case .error: "exclamationmark.triangle"
        case .idle: "circle"
        }
    }

    private var statusColor: Color {
        switch extractor.status {
        case .processing, .startingServer: .orange
        case .finalizing: .orange
        case .done: .green
        case .error: .red
        case .idle: .secondary
        }
    }
}
