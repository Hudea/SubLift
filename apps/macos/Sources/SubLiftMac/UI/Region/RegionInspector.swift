import SwiftUI

/// 10206：Region Inspector 展示纯逻辑（可单测）。
enum RegionInspectorPresentation {

    /// 候选高级几何文本（真实像素字段，不显示工程编号）。
    static func geometryText(for candidate: TextBoxCandidate) -> String {
        "X \(Int(candidate.pixelRect.minX))  Y \(Int(candidate.pixelRect.minY))  宽 \(Int(candidate.pixelRect.width))  高 \(Int(candidate.pixelRect.height))  置信度 \(Int(candidate.confidence * 100))%"
    }

    /// 合并区域摘要（全宽 Y 带）。
    static func mergedSummaryText(_ merged: CGRect) -> String {
        "合并区域：Y \(Int(merged.minY))–\(Int(merged.maxY))，X 全宽 \(Int(merged.width))"
    }

    /// 代表帧信息（真实字段）。
    static func representativeFrameText(seconds: Double?, attempts: Int) -> String? {
        guard let seconds else { return nil }
        return String(format: "代表帧 %.1fs（扫描 %d 帧）", seconds, max(attempts, 1))
    }
}

/// 10206：Region 模式 Inspector 内容。
///
/// 候选多选、重检当前帧、合并摘要与可折叠高级几何；
/// 检测中/空/失败均有明确文案与恢复动作。
struct RegionInspector: View {
    @ObservedObject var regionModel: RegionSelectionModel
    /// 在当前播放头重检。
    var onRedetectAtPlayhead: (() -> Void)? = nil

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            statusContent
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 10)
        .frame(maxHeight: .infinity, alignment: .top)
        .accessibilityElement(children: .contain)
    }

    @ViewBuilder
    private var statusContent: some View {
        switch regionModel.status {
        case .idle:
            Text("等待检测字幕区域…")
                .font(.callout)
                .foregroundStyle(.secondary)

        case .detecting:
            HStack(spacing: 8) {
                ProgressView().controlSize(.small)
                Text(
                    regionModel.sampleAttemptCount > 0
                        ? "多帧扫描字幕区…（已试 \(regionModel.sampleAttemptCount) 帧）"
                        : "多帧扫描字幕区…"
                )
                .font(.callout)
                .foregroundStyle(.secondary)
            }
            .accessibilityElement(children: .combine)
            .accessibilityLabel("正在检测字幕区域")

        case .failed(let message):
            VStack(alignment: .leading, spacing: 8) {
                Label(message, systemImage: "exclamationmark.triangle.fill")
                    .font(.callout)
                    .foregroundStyle(.orange)
                    .accessibilityElement(children: .combine)
                redetectButton
            }

        case .ready:
            if regionModel.candidates.isEmpty {
                VStack(alignment: .leading, spacing: 8) {
                    Text("未检测到文字候选框")
                        .font(.callout)
                        .foregroundStyle(.secondary)
                    Text("可拖动进度条到有字幕的画面后重检。")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    redetectButton
                }
            } else {
                candidateList
                redetectButton
                mergedSummary
                advancedGeometry
            }
        }
    }

    // MARK: - 候选多选

    private var candidateList: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("候选框（\(regionModel.candidates.count)）")
                .font(.caption.weight(.semibold))
                .foregroundStyle(.secondary)

            ScrollView(.vertical, showsIndicators: true) {
                VStack(alignment: .leading, spacing: 4) {
                    ForEach(regionModel.candidates) { candidate in
                        let isSelected = regionModel.selectedIds.contains(candidate.id)
                        Toggle(isOn: Binding(
                            get: { isSelected },
                            set: { regionModel.setSelection(id: candidate.id, selected: $0) }
                        )) {
                            Text(candidate.textPreview)
                                .lineLimit(1)
                                .font(.callout)
                        }
                        .toggleStyle(.checkbox)
                        .accessibilityLabel("候选字幕框 \(candidate.id)")
                        .accessibilityValue(isSelected ? "已选择" : "未选择")
                    }
                }
            }
            .frame(maxHeight: 160)
        }
    }

    // MARK: - 重检

    private var redetectButton: some View {
        Button {
            onRedetectAtPlayhead?()
        } label: {
            Label("重检当前帧", systemImage: "arrow.clockwise")
        }
        .font(.callout)
        .accessibilityHint("先拖动进度条到有字幕的画面")
    }

    // MARK: - 合并摘要

    @ViewBuilder
    private var mergedSummary: some View {
        VStack(alignment: .leading, spacing: 4) {
            if let frameText = RegionInspectorPresentation.representativeFrameText(
                seconds: regionModel.sampleSecondsUsed,
                attempts: regionModel.sampleAttemptCount
            ) {
                Text(frameText)
                    .font(.caption2)
                    .foregroundStyle(.secondary)
            }

            if let merged = regionModel.mergedRegion {
                Label(
                    RegionInspectorPresentation.mergedSummaryText(merged),
                    systemImage: "rectangle.inset.filled"
                )
                .font(.caption2)
                .foregroundStyle(.secondary)
                .accessibilityElement(children: .combine)
            } else {
                Label("请至少选择一个候选框", systemImage: "hand.point.up")
                    .font(.caption2)
                    .foregroundStyle(.orange)
                    .accessibilityElement(children: .combine)
            }
        }
    }

    // MARK: - 可折叠高级几何

    @ViewBuilder
    private var advancedGeometry: some View {
        DisclosureGroup("高级信息") {
            VStack(alignment: .leading, spacing: 6) {
                ForEach(regionModel.candidates) { candidate in
                    HStack(alignment: .top, spacing: 6) {
                        Text("\(candidate.id)")
                            .font(.system(.caption2, design: .monospaced))
                            .foregroundStyle(.secondary)
                            .frame(width: 18, alignment: .trailing)
                        Text(RegionInspectorPresentation.geometryText(for: candidate))
                            .font(.system(.caption2, design: .monospaced))
                            .foregroundStyle(.secondary)
                            .fixedSize(horizontal: false, vertical: true)
                    }
                    .accessibilityElement(children: .combine)
                    .accessibilityLabel("候选 \(candidate.id)：\(RegionInspectorPresentation.geometryText(for: candidate))")
                }
            }
            .padding(.top, 4)
        }
        .font(.caption)
        .accessibilityLabel("高级信息")
    }
}
