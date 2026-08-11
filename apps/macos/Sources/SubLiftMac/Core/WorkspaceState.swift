import Foundation

/// 单窗口 Workspace 的整体主状态。
enum WorkspaceState: String, Equatable, Hashable, CaseIterable, Sendable {
    case empty
    case loading
    case ready
    case regionEditing
    case starting
    case processing
    case finalizing
    case review
    case failed
    case cancelled
}

/// Context Inspector 的当前显示模式。
enum InspectorMode: String, Equatable, Hashable, CaseIterable, Sendable {
    case video
    case region
    case extraction
    case subtitle
}

/// Transcript Panel 的可编辑性阶段。
enum TranscriptAccessMode: String, Equatable, Hashable, CaseIterable, Sendable {
    case readOnly
    case editable
}

/// Toolbar、Menu 与 Context Menu 共用的命令可用性（纯派生值）。
struct WorkspaceCommandAvailability: Equatable, Sendable {
    let canOpen: Bool
    let canEditRegion: Bool
    let isRegionActive: Bool
    let canExtract: Bool
    let canStop: Bool
    let canExport: Bool
    let canEdit: Bool
    let canSplit: Bool
    let canMerge: Bool
    let canShowInspector: Bool

    init(
        canOpen: Bool,
        canEditRegion: Bool,
        isRegionActive: Bool,
        canExtract: Bool,
        canStop: Bool,
        canExport: Bool,
        canEdit: Bool,
        canSplit: Bool,
        canMerge: Bool,
        canShowInspector: Bool
    ) {
        self.canOpen = canOpen
        self.canEditRegion = canEditRegion
        self.isRegionActive = isRegionActive
        self.canExtract = canExtract
        self.canStop = canStop
        self.canExport = canExport
        self.canEdit = canEdit
        self.canSplit = canSplit
        self.canMerge = canMerge
        self.canShowInspector = canShowInspector
    }

    /// 从当前 WorkspaceState 与是否已有 final entries 纯派生命令可用性。
    static func derive(
        state: WorkspaceState,
        hasFinalEntries: Bool,
        totalEntries: Int = 0,
        selectedIndex: Int? = nil
    ) -> WorkspaceCommandAvailability {
        let canOpen = [.empty, .ready, .review, .failed, .cancelled].contains(state)
        let canEditRegion = [.ready, .regionEditing, .review, .failed, .cancelled].contains(state)
        let isRegionActive = (state == .regionEditing)

        let canExtract = [.ready, .review, .failed, .cancelled].contains(state)
        let canStop = [.starting, .processing, .finalizing].contains(state)

        let isMutationAllowed = (state == .review) || ((state == .failed || state == .cancelled) && hasFinalEntries)

        let canExport = isMutationAllowed && hasFinalEntries && totalEntries > 0
        let canEdit = isMutationAllowed && hasFinalEntries && totalEntries > 0

        let hasValidSelection = selectedIndex.map { $0 >= 0 && $0 < totalEntries } ?? false
        let canSplit = isMutationAllowed && hasFinalEntries && hasValidSelection

        let canMerge = isMutationAllowed
            && hasFinalEntries
            && hasValidSelection
            && selectedIndex.map { $0 < totalEntries - 1 } == true

        let canShowInspector = (state != .empty)

        return WorkspaceCommandAvailability(
            canOpen: canOpen,
            canEditRegion: canEditRegion,
            isRegionActive: isRegionActive,
            canExtract: canExtract,
            canStop: canStop,
            canExport: canExport,
            canEdit: canEdit,
            canSplit: canSplit,
            canMerge: canMerge,
            canShowInspector: canShowInspector
        )
    }
}
