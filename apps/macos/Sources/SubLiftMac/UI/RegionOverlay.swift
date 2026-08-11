import SwiftUI

// MARK: - 视觉语义纯逻辑（10206）

/// Region 候选框/合并框的视觉状态派生（不依赖颜色区分状态）。
enum RegionBoxVisual {

    struct Style: Equatable {
        let strokeIsAccent: Bool
        let fillAlpha: Double
        let showsSelectionMark: Bool
        let lineWidth: CGFloat
    }

    /// 状态 → 视觉配置：
    /// - 未选中/hover：secondary / Accent outline；
    /// - 选中：Accent outline + tint + 复选标记；
    /// - merged：更明确的 Accent outline（更粗）。
    static func style(isSelected: Bool, isHovered: Bool, isMerged: Bool) -> Style {
        if isMerged {
            return Style(strokeIsAccent: true, fillAlpha: 0.18, showsSelectionMark: false, lineWidth: 2.5)
        }
        if isSelected {
            return Style(strokeIsAccent: true, fillAlpha: 0.28, showsSelectionMark: true, lineWidth: 2.0)
        }
        if isHovered {
            return Style(strokeIsAccent: true, fillAlpha: 0.10, showsSelectionMark: false, lineWidth: 1.5)
        }
        return Style(strokeIsAccent: false, fillAlpha: 0.06, showsSelectionMark: false, lineWidth: 1.0)
    }
}

/// Overlay 可见性：候选框只在 regionEditing 显示；merged region 始终显示。
enum RegionOverlayVisibility {
    static func showsCandidates(isRegionEditing: Bool) -> Bool {
        isRegionEditing
    }
}

// MARK: - RegionOverlay

/// 10206：视频预览上的区域叠加。
///
/// - 候选框仅在 regionEditing 可见；正常模式只显示最终 merged region。
/// - 使用 secondary/Accent 语义色 + 复选标记 + 描边粗细表达状态，不依赖随机色或工程编号。
struct RegionOverlay: View {
    let candidates: [TextBoxCandidate]
    let selectedIds: Set<Int>
    let mergedRegion: CGRect?
    let isRegionEditing: Bool
    let videoSize: CGSize
    let containerSize: CGSize
    let onToggle: (Int) -> Void

    @State private var hoveredId: Int?

    var body: some View {
        let displayRect = VideoCoordinateMapper.aspectFitDisplayRect(
            videoSize: videoSize,
            containerSize: containerSize
        )

        ZStack {
            if let mergedRegion {
                mergedBox(mergedRegion, displayRect: displayRect)
            }

            if RegionOverlayVisibility.showsCandidates(isRegionEditing: isRegionEditing) {
                ForEach(candidates) { candidate in
                    candidateBox(candidate, displayRect: displayRect)
                }
            }
        }
        .frame(width: containerSize.width, height: containerSize.height)
    }

    // MARK: - Merged region（始终显示，Accent 语义）

    private func mergedBox(_ merged: CGRect, displayRect: CGRect) -> some View {
        let style = RegionBoxVisual.style(isSelected: false, isHovered: false, isMerged: true)
        let viewRect = VideoCoordinateMapper.videoRectToViewRect(
            merged,
            videoSize: videoSize,
            displayRect: displayRect
        )
        return Rectangle()
            .fill(Color.accentColor.opacity(style.fillAlpha))
            .overlay(
                Rectangle()
                    .stroke(Color.accentColor, style: StrokeStyle(lineWidth: style.lineWidth, dash: [6, 4]))
            )
            .frame(width: max(viewRect.width, 4), height: max(viewRect.height, 4))
            .position(x: viewRect.midX, y: viewRect.midY)
            .allowsHitTesting(false)
            .accessibilityLabel("合并字幕区域，Y \(Int(merged.minY)) 至 Y \(Int(merged.maxY))")
    }

    // MARK: - Candidate box（仅 regionEditing）

    @ViewBuilder
    private func candidateBox(_ candidate: TextBoxCandidate, displayRect: CGRect) -> some View {
        let viewRect = VideoCoordinateMapper.videoRectToViewRect(
            candidate.pixelRect,
            videoSize: videoSize,
            displayRect: displayRect
        )
        let isSelected = selectedIds.contains(candidate.id)
        let isHovered = hoveredId == candidate.id
        let style = RegionBoxVisual.style(isSelected: isSelected, isHovered: isHovered, isMerged: false)
        let strokeColor = style.strokeIsAccent ? Color.accentColor : Color.secondary

        ZStack(alignment: .topLeading) {
            Rectangle()
                .fill(strokeColor.opacity(style.fillAlpha))
                .overlay(
                    Rectangle()
                        .stroke(strokeColor, lineWidth: style.lineWidth)
                )

            if style.showsSelectionMark {
                Image(systemName: "checkmark.circle.fill")
                    .font(.system(size: 13))
                    .foregroundStyle(Color.accentColor)
                    .background(Circle().fill(.white.opacity(0.9)))
                    .padding(3)
                    .accessibilityHidden(true)
            }
        }
        .frame(width: max(viewRect.width, 12), height: max(viewRect.height, 12))
        .position(x: viewRect.midX, y: viewRect.midY)
        .contentShape(Rectangle())
        .onHover { hovering in
            hoveredId = hovering ? candidate.id : nil
        }
        .onTapGesture { onToggle(candidate.id) }
        .accessibilityElement(children: .ignore)
        .accessibilityLabel("候选字幕框，\(isSelected ? "已选择" : "未选择")")
        .accessibilityAddTraits(isSelected ? .isSelected : [])
        .accessibilityHint("点击切换选择")
    }
}

// MARK: - PreviewRegionContainer

/// 预览容器：在视频画面上层绘制区域叠加。
struct PreviewRegionContainer<Content: View>: View {
    @ObservedObject var regionModel: RegionSelectionModel
    let videoWidth: Int
    let videoHeight: Int
    /// 是否处于 Region Editing（候选框仅此时显示）。
    var isRegionEditing: Bool = false
    @ViewBuilder let content: () -> Content

    var body: some View {
        GeometryReader { geometry in
            ZStack {
                content()
                    .frame(width: geometry.size.width, height: geometry.size.height)

                if videoWidth > 0, videoHeight > 0, !regionModel.candidates.isEmpty || regionModel.mergedRegion != nil {
                    RegionOverlay(
                        candidates: regionModel.candidates,
                        selectedIds: regionModel.selectedIds,
                        mergedRegion: regionModel.mergedRegion,
                        isRegionEditing: isRegionEditing,
                        videoSize: CGSize(width: videoWidth, height: videoHeight),
                        containerSize: geometry.size,
                        onToggle: { regionModel.toggleSelection(id: $0) }
                    )
                }
            }
        }
    }
}
