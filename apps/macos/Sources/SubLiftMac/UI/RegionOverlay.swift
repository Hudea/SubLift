import SwiftUI

/// feat-022：在视频预览上叠加 Vision 候选框（按颜色区分）并支持点选。
struct RegionOverlay: View {
    let candidates: [TextBoxCandidate]
    let selectedIds: Set<Int>
    let mergedRegion: CGRect?
    let videoSize: CGSize
    let containerSize: CGSize
    let onToggle: (Int) -> Void

    var body: some View {
        let displayRect = VideoCoordinateMapper.aspectFitDisplayRect(
            videoSize: videoSize,
            containerSize: containerSize
        )

        ZStack {
            if let mergedRegion {
                let viewRect = VideoCoordinateMapper.videoRectToViewRect(
                    mergedRegion,
                    videoSize: videoSize,
                    displayRect: displayRect
                )
                Rectangle()
                    .fill(Color.green.opacity(0.12))
                    .overlay(
                        Rectangle()
                            .stroke(Color.green.opacity(0.85), style: StrokeStyle(lineWidth: 2, dash: [6, 4]))
                    )
                    .frame(width: viewRect.width, height: viewRect.height)
                    .position(x: viewRect.midX, y: viewRect.midY)
                    .allowsHitTesting(false)
            }

            ForEach(candidates) { candidate in
                candidateBox(candidate, displayRect: displayRect)
            }
        }
        .frame(width: containerSize.width, height: containerSize.height)
    }

    @ViewBuilder
    private func candidateBox(_ candidate: TextBoxCandidate, displayRect: CGRect) -> some View {
        let viewRect = VideoCoordinateMapper.videoRectToViewRect(
            candidate.pixelRect,
            videoSize: videoSize,
            displayRect: displayRect
        )
        let palette = RegionBoxPalette.color(for: candidate.colorIndex)
        let color = Color(red: palette.red, green: palette.green, blue: palette.blue)
        let isSelected = selectedIds.contains(candidate.id)

        ZStack(alignment: .topLeading) {
            Rectangle()
                .fill(color.opacity(isSelected ? 0.28 : 0.08))
                .overlay(
                    Rectangle()
                        .stroke(color.opacity(isSelected ? 0.95 : 0.65), lineWidth: isSelected ? 2.5 : 1.5)
                )

            Text("\(candidate.id)")
                .font(.system(size: 11, weight: .bold, design: .rounded))
                .foregroundStyle(.white)
                .padding(.horizontal, 5)
                .padding(.vertical, 2)
                .background(color.opacity(0.9))
                .clipShape(RoundedRectangle(cornerRadius: 4))
                .padding(4)
        }
        .frame(width: max(viewRect.width, 8), height: max(viewRect.height, 8))
        .position(x: viewRect.midX, y: viewRect.midY)
        .contentShape(Rectangle())
        .onTapGesture { onToggle(candidate.id) }
    }
}

/// feat-022：预览容器，在视频画面上层绘制区域叠加。
struct PreviewRegionContainer<Content: View>: View {
    @ObservedObject var regionModel: RegionSelectionModel
    let videoWidth: Int
    let videoHeight: Int
    @ViewBuilder let content: () -> Content

    var body: some View {
        GeometryReader { geometry in
            ZStack {
                content()
                    .frame(width: geometry.size.width, height: geometry.size.height)

                if videoWidth > 0, videoHeight > 0, !regionModel.candidates.isEmpty {
                    RegionOverlay(
                        candidates: regionModel.candidates,
                        selectedIds: regionModel.selectedIds,
                        mergedRegion: regionModel.mergedRegion,
                        videoSize: CGSize(width: videoWidth, height: videoHeight),
                        containerSize: geometry.size,
                        onToggle: { regionModel.toggleSelection(id: $0) }
                    )
                }
            }
        }
    }
}

/// feat-022：候选框多选列表（颜色与预览一致）。
struct RegionCandidateList: View {
    @ObservedObject var model: RegionSelectionModel

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            header

            switch model.status {
            case .idle:
                Text("等待检测字幕区域...")
                    .font(.caption)
                    .foregroundStyle(.secondary)
            case .detecting:
                HStack(spacing: 8) {
                    ProgressView().controlSize(.small)
                    Text("Vision 检测文字框...")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            case .failed(let message):
                Text(message)
                    .font(.caption)
                    .foregroundStyle(.red)
            case .ready:
                if model.candidates.isEmpty {
                    Text("无候选框")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                } else {
                    candidateRows
                    mergedSummary
                }
            }
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
    }

    @ViewBuilder
    private var header: some View {
        HStack {
            Label("字幕区域", systemImage: "viewfinder")
                .font(.caption.weight(.semibold))
            Spacer()
            if model.status == .ready, !model.candidates.isEmpty {
                Text("点预览或列表多选")
                    .font(.caption2)
                    .foregroundStyle(.secondary)
            }
        }
    }

    @ViewBuilder
    private var candidateRows: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack(spacing: 8) {
                ForEach(model.candidates) { candidate in
                    let palette = RegionBoxPalette.color(for: candidate.colorIndex)
                    let color = Color(red: palette.red, green: palette.green, blue: palette.blue)
                    let isSelected = model.selectedIds.contains(candidate.id)

                    Toggle(isOn: Binding(
                        get: { isSelected },
                        set: { model.setSelection(id: candidate.id, selected: $0) }
                    )) {
                        HStack(spacing: 6) {
                            Circle()
                                .fill(color)
                                .frame(width: 8, height: 8)
                            Text("#\(candidate.id)")
                                .font(.system(.caption, design: .monospaced))
                            Text(candidate.textPreview)
                                .lineLimit(1)
                                .font(.caption)
                        }
                    }
                    .toggleStyle(.checkbox)
                }
            }
        }
    }

    @ViewBuilder
    private var mergedSummary: some View {
        if let merged = model.mergedRegion {
            Text(
                String(
                    format: "合并区域：Y %.0f–%.0f，X 全宽 %.0f",
                    merged.minY,
                    merged.maxY,
                    merged.width
                )
            )
            .font(.caption2)
            .foregroundStyle(.secondary)
        } else {
            Text("请至少选择一个候选框")
                .font(.caption2)
                .foregroundStyle(.orange)
        }
    }
}