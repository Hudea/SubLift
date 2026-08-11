import Combine
import Foundation

/// 单窗口 Session 协调模型。
///
/// 组合现有 focused Core models（PlayerModel、SubtitleExtractor、SubtitleEditor、
/// VideoMetadataLoader、RegionSelectionModel），统一管理 Workspace 状态、
/// Inspector 模式、Transcript 编辑权限和 Command availability。
@MainActor
final class WorkspaceModel: ObservableObject {

    typealias MetadataRequest = @MainActor (URL) async -> VideoMetadata

    // MARK: - Published Properties

    @Published private(set) var state: WorkspaceState = .empty
    @Published private(set) var currentVideoURL: URL?
    @Published var inspectorMode: InspectorMode = .video
    @Published var isInspectorPresented: Bool = false
    @Published private(set) var errorMessage: String?
    @Published private(set) var hasFinalEntries: Bool = false

    /// 当前 Session 的 generation token；打开新视频时更新。
    @Published private(set) var sessionToken: UUID = UUID()

    /// 当前活动提取 job 的 token；任务结束/取消时清空。
    @Published private(set) var jobToken: UUID?

    // MARK: - Focused Core Models

    let playerModel: PlayerModel
    let extractor: SubtitleExtractor
    let editor: SubtitleEditor
    let metadataLoader: VideoMetadataLoader
    let regionModel: RegionSelectionModel

    // MARK: - Private Fields

    private var cancellables = Set<AnyCancellable>()
    private var priorStateBeforeRegionEditing: WorkspaceState = .ready
    private var metadataTask: Task<Void, Never>?
    private let metadataRequest: MetadataRequest
    private var retainedFinalEntries: [SubtitleEntryData]?
    private var liveEntryCount = 0

    // MARK: - Initializer

    init(
        playerModel: PlayerModel? = nil,
        extractor: SubtitleExtractor? = nil,
        editor: SubtitleEditor? = nil,
        metadataLoader: VideoMetadataLoader? = nil,
        regionModel: RegionSelectionModel? = nil,
        metadataRequest: MetadataRequest? = nil
    ) {
        let resolvedMetadataLoader = metadataLoader ?? VideoMetadataLoader()
        self.playerModel = playerModel ?? PlayerModel()
        self.extractor = extractor ?? SubtitleExtractor()
        self.editor = editor ?? SubtitleEditor()
        self.metadataLoader = resolvedMetadataLoader
        self.regionModel = regionModel ?? RegionSelectionModel()
        self.metadataRequest = metadataRequest ?? { url in
            await resolvedMetadataLoader.fetch(url: url)
        }

        setupSubscriptions()
    }

    deinit {
        metadataTask?.cancel()
    }

    // MARK: - Subscriptions

    private func setupSubscriptions() {
        // 转发子 ObservableObject 的 objectWillChange，避免嵌套刷新丢失。
        playerModel.objectWillChange
            .sink { [weak self] _ in self?.objectWillChange.send() }
            .store(in: &cancellables)

        extractor.objectWillChange
            .sink { [weak self] _ in self?.objectWillChange.send() }
            .store(in: &cancellables)

        editor.objectWillChange
            .sink { [weak self] _ in self?.objectWillChange.send() }
            .store(in: &cancellables)

        metadataLoader.objectWillChange
            .sink { [weak self] _ in self?.objectWillChange.send() }
            .store(in: &cancellables)

        regionModel.objectWillChange
            .sink { [weak self] _ in self?.objectWillChange.send() }
            .store(in: &cancellables)

        // 监听 playerModel 时间同步 editor currentId
        playerModel.$currentMs
            .sink { [weak self] ms in
                self?.editor.updateCurrent(atMs: ms)
            }
            .store(in: &cancellables)

        // 监听 extractor 状态联动
        extractor.$status
            .sink { [weak self] status in
                guard let self, let currentJobToken = self.jobToken else { return }
                switch status {
                case .startingServer:
                    self.handleExtractionStarting(jobToken: currentJobToken)
                case .processing(let pct, let currentFrame, let totalFrames):
                    self.handleExtractionProgress(progress: pct, frameCount: currentFrame, totalFrames: totalFrames, jobToken: currentJobToken)
                case .finalizing:
                    self.handleExtractionFinalizing(jobToken: currentJobToken)
                case .done:
                    self.handleExtractionFinalEntries(self.extractor.entries, jobToken: currentJobToken)
                case .error(let msg):
                    self.handleExtractionError(msg, jobToken: currentJobToken)
                case .idle:
                    break
                }
            }
            .store(in: &cancellables)

        // 流式 entries 只在当前 job 的只读阶段投影到 editor；final 由 status.done 原子替换。
        extractor.$entries
            .sink { [weak self] entries in
                guard let self, let currentJobToken = self.jobToken else { return }
                self.handleExtractionIncrementalEntries(entries, jobToken: currentJobToken)
            }
            .store(in: &cancellables)
    }

    // MARK: - Derived State

    var transcriptAccessMode: TranscriptAccessMode {
        switch state {
        case .review:
            return hasFinalEntries ? .editable : .readOnly
        case .failed, .cancelled:
            return hasFinalEntries ? .editable : .readOnly
        default:
            return .readOnly
        }
    }

    var selectedIndex: Int? {
        guard let selectedId = editor.selectedId else { return nil }
        return editor.entries.firstIndex(where: { $0.id == selectedId })
    }

    var commandAvailability: WorkspaceCommandAvailability {
        WorkspaceCommandAvailability.derive(
            state: state,
            hasFinalEntries: hasFinalEntries,
            totalEntries: editor.entries.count,
            selectedIndex: selectedIndex
        )
    }

    // MARK: - Public Intent & Event Handlers

    /// 打开或切换视频：重置 Session 临时状态（不改 Settings），生成新 sessionToken 并加载 metadata。
    @discardableResult
    func openVideo(url: URL) -> Bool {
        guard commandAvailability.canOpen else { return false }

        metadataTask?.cancel()

        let newSessionToken = UUID()
        sessionToken = newSessionToken
        jobToken = nil
        currentVideoURL = url
        errorMessage = nil
        hasFinalEntries = false
        retainedFinalEntries = nil
        liveEntryCount = 0

        // 清理旧 Session 临时状态
        editor.clear()
        regionModel.clear()
        metadataLoader.clear()
        playerModel.url = url

        state = .loading
        setInspectorMode(.video)

        // 捕获请求发起时的 token；只在当前 Session 仍处于 loading 时发布结果。
        let request = metadataRequest
        metadataTask = Task { [weak self, newSessionToken] in
            let metadata = await request(url)
            guard let self, !Task.isCancelled else { return }
            guard newSessionToken == self.sessionToken, self.state == .loading else { return }
            self.metadataLoader.publish(metadata)
            self.handleMetadataLoaded(metadata, sessionToken: newSessionToken)
        }

        return true
    }

    /// metadata 加载完成；校验 token 防止迟到响应污染新 Session。
    func handleMetadataLoaded(_ metadata: VideoMetadata, sessionToken token: UUID) {
        guard token == sessionToken else { return }
        guard state == .loading else { return }
        state = .ready
        errorMessage = nil

        if let url = currentVideoURL {
            Task {
                await regionModel.detect(
                    url: url,
                    videoWidth: metadata.width,
                    videoHeight: metadata.height,
                    durationMs: metadata.durationMs
                )
            }
        }
    }

    /// metadata 加载失败。
    func handleMetadataFailed(_ error: String, sessionToken token: UUID) {
        guard token == sessionToken else { return }
        guard state == .loading else { return }
        errorMessage = error
        state = .failed
    }

    /// 进入 Region Editing 状态。
    func enterRegionEditing() {
        guard currentVideoURL != nil else { return }
        guard [.ready, .review, .failed, .cancelled].contains(state) else { return }
        priorStateBeforeRegionEditing = state
        state = .regionEditing
        setInspectorMode(.region)
    }

    /// 退出 Region Editing 状态。
    func exitRegionEditing() {
        guard state == .regionEditing else { return }
        state = priorStateBeforeRegionEditing
        setInspectorMode(editor.selectedId != nil ? .subtitle : .video)
    }

    /// 开始提取。
    func startExtraction(
        engine: OcrEngineName = .vision,
        quality: SamplingQuality = .fast
    ) {
        guard let url = currentVideoURL else { return }
        guard [.ready, .review, .failed, .cancelled].contains(state) else { return }
        guard !extractor.isRunning else { return }

        let token = UUID()
        retainedFinalEntries = hasFinalEntries ? editor.exportEntries() : nil
        editor.clear()
        liveEntryCount = 0
        jobToken = token
        hasFinalEntries = false
        errorMessage = nil
        state = .starting
        setInspectorMode(.extraction)

        extractor.extract(
            videoURL: url,
            fps: quality.sampleFps,
            engine: engine,
            regionBox: regionModel.regionBoxForIPC(),
            subtitleProfile: regionModel.subtitleProfileForIPC()
        )
    }

    // 校验合法前置状态，拒绝乱序倒退。
    func handleExtractionStarting(jobToken token: UUID) {
        guard token == jobToken else { return }
        guard [.starting, .ready, .review, .failed, .cancelled].contains(state) else { return }
        state = .starting
    }

    func handleExtractionProgress(progress: Double, frameCount: Int, totalFrames: Int, jobToken token: UUID) {
        guard token == jobToken else { return }
        guard [.starting, .processing].contains(state) else { return }
        state = .processing
    }

    func handleExtractionFinalizing(jobToken token: UUID) {
        guard token == jobToken else { return }
        guard [.starting, .processing, .finalizing].contains(state) else { return }
        state = .finalizing
    }

    /// 接收当前 job 的流式快照；只追加新增条目，不将其标记为最终结果。
    func handleExtractionIncrementalEntries(_ entries: [SubtitleEntryData], jobToken token: UUID) {
        guard token == jobToken else { return }
        guard [.starting, .processing, .finalizing].contains(state) else { return }

        if entries.count < liveEntryCount {
            editor.clear()
            liveEntryCount = 0
        }
        guard entries.count > liveEntryCount else { return }

        for entry in entries.dropFirst(liveEntryCount) {
            editor.appendIncremental(entry)
        }
        liveEntryCount = entries.count
    }

    /// 最终 entries 原子落地，切换到 review 阶段并开放编辑。
    func handleExtractionFinalEntries(_ entries: [SubtitleEntryData], jobToken token: UUID) {
        guard token == jobToken else { return }
        guard [.starting, .processing, .finalizing].contains(state) else { return }
        editor.load(entries)
        jobToken = nil
        retainedFinalEntries = nil
        liveEntryCount = 0
        hasFinalEntries = true
        state = .review

        setInspectorMode(editor.selectedId != nil ? .subtitle : .video)
    }

    func handleExtractionError(_ error: String, jobToken token: UUID) {
        guard token == jobToken else { return }
        guard [.starting, .processing, .finalizing].contains(state) else { return }
        errorMessage = error
        jobToken = nil
        restoreRetainedFinalEntries()
        state = .failed
    }

    /// 取消当前提取。
    func cancelExtraction() {
        guard [.starting, .processing, .finalizing].contains(state) else { return }
        jobToken = nil
        extractor.cancel()
        restoreRetainedFinalEntries()
        state = .cancelled
    }

    /// 从 failed 或 cancelled 恢复到安全状态。
    func retry() {
        guard [.failed, .cancelled].contains(state) else { return }
        errorMessage = nil
        if hasFinalEntries {
            state = .review
        } else if currentVideoURL != nil {
            state = .ready
        } else {
            state = .empty
        }
    }

    /// 选择字幕并自动调整 InspectorMode，但不改变 isInspectorPresented。
    func selectSubtitle(id: UUID?) {
        guard id == nil || editor.entries.contains(where: { $0.id == id }) else { return }
        editor.selectedId = id
        if id != nil {
            setInspectorMode(.subtitle)
        }
    }

    /// 切换 InspectorMode，不强制改变 isInspectorPresented 开关。
    func setInspectorMode(_ mode: InspectorMode) {
        inspectorMode = mode
    }

    /// 显式开/关 Inspector 面板。
    func toggleInspector() {
        isInspectorPresented.toggle()
    }

    func setInspectorPresented(_ presented: Bool) {
        isInspectorPresented = presented
    }

    // MARK: - Private Helpers

    private func restoreRetainedFinalEntries() {
        liveEntryCount = 0
        if let retainedFinalEntries {
            editor.load(retainedFinalEntries)
            hasFinalEntries = true
        } else {
            editor.clear()
            hasFinalEntries = false
        }
        retainedFinalEntries = nil
    }
}
