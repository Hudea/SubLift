import Foundation
import SwiftUI

/// Timeline 范围标签：省略毫秒与不足一小时的冗余小时位，降低紧凑窗口中的换行风险。
enum TimelineRangePresentation {

    static func text(startMs: Int, endMs: Int) -> String {
        guard endMs > startMs else { return "—" }
        return "\(component(startMs))–\(component(endMs))"
    }

    private static func component(_ milliseconds: Int) -> String {
        let totalSeconds = max(0, milliseconds) / 1_000
        let seconds = totalSeconds % 60
        let totalMinutes = totalSeconds / 60
        let minutes = totalMinutes % 60
        let hours = totalMinutes / 60
        if hours > 0 {
            return String(format: "%d:%02d:%02d", hours, minutes, seconds)
        }
        return String(format: "%d:%02d", totalMinutes, seconds)
    }
}

/// 10310：Subtitle Timeline 条带（28–40pt）。
///
/// 条目块按确定性时间几何映射；极短条目有最小命中宽度；点击条带 seek；
/// Previous/Next 按钮复用 TimelineNavigation 纯逻辑（seek 由调用方闭包提供）。
/// 不实现波形、多轨、缩放或拖拽改时。
struct SubtitleTimelineView: View {
    let entries: [SubtitleEntry]
    /// 当前播放时间（ms）。
    let currentMs: Int
    /// 视频总时长（ms）；>0 时按视频时长映射（验收合同），否则回退到 entries 范围。
    let videoDurationMs: Int
    let onSeek: (Int) -> Void

    /// 实际映射范围：优先视频时长，回退 entries 首尾。
    private var effectiveRange: TimelineGeometry.Range {
        if videoDurationMs > 0 {
            return TimelineGeometry.Range(startMs: 0, durationMs: videoDurationMs)
        }
        return TimelineGeometry.range(from: entries)
    }

    var body: some View {
        HStack(spacing: 10) {
            Button(action: goPrevious) {
                Image(systemName: "backward.end.fill")
            }
            .buttonStyle(.borderless)
            .disabled(previousTarget == nil)
            .help("上一条字幕")
            .accessibilityLabel("上一条字幕")

            Button(action: goNext) {
                Image(systemName: "forward.end.fill")
            }
            .buttonStyle(.borderless)
            .disabled(nextTarget == nil)
            .help("下一条字幕")
            .accessibilityLabel("下一条字幕")

            track

            Text(timeRangeText)
                .font(.system(.caption2, design: .monospaced))
                .monospacedDigit()
                .foregroundStyle(.secondary)
                .lineLimit(1)
                .allowsTightening(true)
                .minimumScaleFactor(0.75)
                .frame(
                    minWidth: 68,
                    idealWidth: 86,
                    maxWidth: 96,
                    minHeight: nil,
                    idealHeight: nil,
                    maxHeight: nil,
                    alignment: .trailing
                )
                .layoutPriority(1)
                .accessibilityLabel("时间范围 \(timeRangeText)")
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 6)
        .frame(height: TimelineGeometry.barHeight + 12)
        .accessibilityElement(children: .contain)
    }

    // MARK: - Track

    private var track: some View {
        GeometryReader { geometry in
            let range = effectiveRange
            let trackWidth = geometry.size.width

            ZStack(alignment: .topLeading) {
                // 背景轨道
                RoundedRectangle(cornerRadius: 3)
                    .fill(Color(nsColor: .controlBackgroundColor))
                    .overlay(
                        RoundedRectangle(cornerRadius: 3)
                            .stroke(Color(nsColor: .separatorColor), lineWidth: 0.5)
                    )

                // 条目块（点击 seek 到条目 start；VoiceOver 双击激活）
                ForEach(Array(entries.enumerated()), id: \.element.id) { index, entry in
                    let x = TimelineGeometry.xPosition(timeMs: entry.startMs, range: range, width: trackWidth)
                    let width = TimelineGeometry.entryWidth(
                        startMs: entry.startMs,
                        endMs: entry.endMs,
                        range: range,
                        trackWidth: trackWidth
                    )
                    RoundedRectangle(cornerRadius: 2)
                        .fill(Color.accentColor.opacity(0.45))
                        .frame(width: width, height: TimelineGeometry.barHeight - 6)
                        .position(x: x + width / 2, y: (TimelineGeometry.barHeight - 6) / 2)
                        .contentShape(Rectangle())
                        .onTapGesture { onSeek(entry.startMs) }
                        .accessibilityElement(children: .ignore)
                        .accessibilityLabel("字幕 \(index + 1)，\(TimeFormatter.formatMs(entry.startMs)) 到 \(TimeFormatter.formatMs(entry.endMs))")
                        .accessibilityAddTraits(.isButton)
                        .accessibilityHint("跳转到该字幕")
                        .accessibilityAction { onSeek(entry.startMs) }
                }

                // 播放头
                let playheadX = TimelineGeometry.xPosition(timeMs: currentMs, range: range, width: trackWidth)
                Rectangle()
                    .fill(Color.accentColor)
                    .frame(width: 2, height: TimelineGeometry.barHeight)
                    .position(x: playheadX, y: TimelineGeometry.barHeight / 2)
                    .accessibilityLabel("播放位置")
            }
            .frame(height: TimelineGeometry.barHeight)
            .contentShape(Rectangle())
            .onTapGesture { location in
                // 空白处点击：按时间逆映射 seek。
                guard range.durationMs > 0 else { return }
                let fraction = Double(min(1, max(0, location.x / max(trackWidth, 1))))
                let targetMs = range.startMs + Int(fraction * Double(range.durationMs))
                onSeek(targetMs)
            }
        }
        .frame(height: TimelineGeometry.barHeight)
    }

    // MARK: - Navigation

    private var previousTarget: Int? {
        TimelineNavigation.previousEntryIndex(currentMs: currentMs, entries: entries)
    }

    private var nextTarget: Int? {
        TimelineNavigation.nextEntryIndex(currentMs: currentMs, entries: entries)
    }

    private func goPrevious() {
        guard let index = previousTarget else { return }
        onSeek(entries[index].startMs)
    }

    private func goNext() {
        guard let index = nextTarget else { return }
        onSeek(entries[index].startMs)
    }

    private var timeRangeText: String {
        let range = effectiveRange
        let endMs = range.startMs + range.durationMs
        return TimelineRangePresentation.text(startMs: range.startMs, endMs: endMs)
    }
}
