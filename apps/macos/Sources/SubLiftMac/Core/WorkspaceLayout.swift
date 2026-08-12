import CoreGraphics
import Foundation

/// 10104：Workspace 布局纯逻辑（无状态、可测）。
///
/// 合同（docs/design_ui/interaction-state-spec.md §7）：
/// - Video 60–65% / Transcript 35–40%；
/// - 变窄时先回收 Inspector 预留位，再把 Transcript 缩至不低于约 320pt；
/// - 宽窗口不把 Transcript 无上限拉宽。
enum WorkspaceLayout {

    /// Transcript 面板最小宽度（设计合同约 320pt）。
    static let transcriptMinWidth: CGFloat = 320

    /// Inspector 预留位（10105 使用；设计范围 280–360pt）。
    static let inspectorReservedWidth: CGFloat = 300

    /// Transcript 目标占比（38%，落在 35–40% 合同内）。
    static let transcriptTargetRatio: CGFloat = 0.38

    /// Split 计算结果。
    struct SplitWidths: Equatable {
        let video: CGFloat
        let transcript: CGFloat

        var total: CGFloat { video + transcript }
    }

    /// 已把 divider 计入容器预算的最终两栏宽度。
    ///
    /// `proposedLeftWidth` 可以是窗口缩放或 Inspector 开关前缓存的旧值；每次布局都重新
    /// clamp，确保任何缓存值都不会让两栏越出当前容器。
    struct ResolvedColumns: Equatable {
        let left: CGFloat
        let right: CGFloat
        let divider: CGFloat

        var total: CGFloat { left + divider + right }
    }

    /// 计算 Video / Transcript 宽度。
    ///
    /// - Parameters:
    ///   - containerWidth: 内容区总宽度。
    ///   - inspectorWidth: Inspector 当前占用宽度（0 = 未显示）；先从此回收。
    /// - Returns: 两侧宽度（之和 = 扣除 Inspector 后的可用宽度）。
    static func splitWidths(
        containerWidth: CGFloat,
        inspectorWidth: CGFloat = 0
    ) -> SplitWidths {
        let usable = max(containerWidth - inspectorWidth, 0)
        // Transcript 保底 320pt；比例在 38%（35–40% 内）。
        let transcript = min(max(usable * transcriptTargetRatio, transcriptMinWidth), usable)
        let video = usable - transcript
        return SplitWidths(video: max(video, 0), transcript: max(transcript, 0))
    }

    /// 用户拖动 Split divider 后对左栏宽度的 clamp：
    /// - 空间充足时：左栏 ∈ [leftMin, container − rightMin]（右栏保底 rightMin）；
    /// - 空间不足时（container < leftMin + rightMin）：优先保证右栏 ≥ rightMin，
    ///   左栏让位（可低于 leftMin），避免 HStack 溢出。
    static func clampedLeftWidth(
        proposed: CGFloat,
        containerWidth: CGFloat,
        leftMin: CGFloat,
        rightMin: CGFloat
    ) -> CGFloat {
        let upperBound = max(containerWidth - rightMin, 0)
        let lowerBound = min(leftMin, upperBound)
        return min(max(proposed, lowerBound), max(upperBound, lowerBound))
    }

    /// 将 divider 与两栏完整分配到当前容器中，避免右栏用最小宽度反向撑破 HStack。
    static func resolvedColumns(
        proposedLeftWidth: CGFloat,
        containerWidth: CGFloat,
        dividerWidth: CGFloat,
        leftMin: CGFloat,
        rightMin: CGFloat
    ) -> ResolvedColumns {
        let resolvedDivider = min(max(dividerWidth, 0), max(containerWidth, 0))
        let panesWidth = max(containerWidth - resolvedDivider, 0)
        let left = clampedLeftWidth(
            proposed: proposedLeftWidth,
            containerWidth: panesWidth,
            leftMin: leftMin,
            rightMin: rightMin
        )
        return ResolvedColumns(
            left: left,
            right: max(panesWidth - left, 0),
            divider: resolvedDivider
        )
    }
}
