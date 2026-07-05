import AVFoundation
import AppKit
import Combine
import SwiftUI

// MARK: - PlayerModel

/// AVPlayer 状态桥接器：把 AVPlayer 的播放状态、当前时间暴露为 @Published。
final class PlayerModel: ObservableObject {
    @Published private(set) var currentMs: Int = 0
    @Published private(set) var durationMs: Int = 0
    @Published private(set) var isPlaying: Bool = false
    @Published private(set) var player: AVPlayer?
    @Published private(set) var loadFailed: Bool = false
    /// mkv 等 AVPlayer 不支持的格式：用 ffmpeg 抽首帧作为静态预览。
    @Published private(set) var fallbackPreview: NSImage?

    var url: URL? {
        didSet {
            if let url = url {
                load(url: url)
            } else {
                unload()
            }
        }
    }

    private var timeObserverToken: Any?
    private var statusObservation: NSKeyValueObservation?
    private var rateObservation: NSKeyValueObservation?

    deinit {
        if let token = timeObserverToken {
            player?.removeTimeObserver(token)
        }
        statusObservation?.invalidate()
        rateObservation?.invalidate()
    }

    // MARK: - 加载

    private func load(url: URL) {
        // 移除旧观察器
        if let token = timeObserverToken {
            player?.removeTimeObserver(token)
            self.timeObserverToken = nil
        }
        statusObservation?.invalidate()
        rateObservation?.invalidate()

        let item = AVPlayerItem(url: url)
        let newPlayer: AVPlayer
        if let existing = player {
            existing.replaceCurrentItem(with: item)
            newPlayer = existing
        } else {
            newPlayer = AVPlayer(playerItem: item)
            player = newPlayer
        }

        loadFailed = false
        fallbackPreview = nil
        observePlayer(newPlayer)
        observeItem(item)

        currentMs = 0
        durationMs = 0
    }

    private func unload() {
        player?.pause()
        player?.replaceCurrentItem(with: nil)
        currentMs = 0
        durationMs = 0
        isPlaying = false
        fallbackPreview = nil
    }

    // MARK: - 观察器

    private func observePlayer(_ player: AVPlayer) {
        // 10Hz 时间更新
        let interval = CMTime(seconds: 0.1, preferredTimescale: 600)
        timeObserverToken = player.addPeriodicTimeObserver(
            forInterval: interval,
            queue: .main
        ) { [weak self] time in
            self?.currentMs = Int(CMTimeGetSeconds(time) * 1000)
        }

        // 播放速率变化 → isPlaying
        rateObservation = player.observe(\.rate, options: [.new]) { [weak self] player, _ in
            DispatchQueue.main.async {
                self?.isPlaying = player.rate > 0
            }
        }
    }

    private func observeItem(_ item: AVPlayerItem) {
        statusObservation = item.observe(\.status, options: [.new]) { [weak self] item, _ in
            DispatchQueue.main.async {
                if item.status == .readyToPlay {
                    self?.loadFailed = false
                    self?.fallbackPreview = nil
                    let secs = CMTimeGetSeconds(item.duration)
                    if secs.isFinite {
                        self?.durationMs = Int(secs * 1000)
                    }
                } else if item.status == .failed {
                    self?.loadFailed = true
                    // AVPlayer 失败时（如 mkv），用 ffmpeg 兜底预览
                    self?.startFallbackMode()
                }
            }
        }
    }

    // MARK: - ffmpeg 兜底预览模式（mkv）

    /// 进入 ffmpeg 兜底模式：用 ffprobe 取时长，ffmpeg 抽首帧。
    private func startFallbackMode() {
        guard let url = url,
              FfmpegDetector.detect() != nil else { return }

        // 用 ffprobe 取时长
        Task.detached(priority: .userInitiated) { [weak self] in
            let durationMs = FfmpegFrameSampler.probeDurationMs(url: url)
            let firstFrame = FfmpegFrameSampler.captureFrameAt(url: url, seconds: 0)

            DispatchQueue.main.async {
                self?.durationMs = durationMs
                self?.fallbackPreview = firstFrame
            }
        }
    }

    /// ffmpeg 兜底模式下的 seek：用 ffmpeg 抽指定时间点的帧。
    /// - Parameter ms: 目标毫秒位置
    func fallbackSeek(toMs ms: Int) {
        guard loadFailed, let url = url else { return }
        currentMs = ms

        // 节流：避免拖动进度条时频繁 spawn ffmpeg
        fallbackSeekTask?.cancel()
        fallbackSeekTask = Task.detached(priority: .userInitiated) { [weak self] in
            // 间隔 100ms，避免拖动时每帧都 spawn
            try? await Task.sleep(nanoseconds: 100_000_000)
            if Task.isCancelled { return }

            let secs = Double(ms) / 1000.0
            let image = FfmpegFrameSampler.captureFrameAt(url: url, seconds: secs)

            await MainActor.run {
                self?.fallbackPreview = image
            }
        }
    }

    private var fallbackSeekTask: Task<Void, Never>?

    // MARK: - 播放控制

    func play() { player?.play() }
    func pause() { player?.pause() }
    func togglePlay() { isPlaying ? pause() : play() }

    /// 跳转到指定毫秒位置。
    func seek(toMs ms: Int) {
        let target = CMTime(seconds: Double(ms) / 1000.0, preferredTimescale: 600)
        player?.seek(to: target)
        currentMs = ms
    }
}

// MARK: - VideoPreview (NSViewRepresentable)

/// SwiftUI ↔ AVPlayerLayer 桥接。
/// feat-017：播放预览；feat-022 将在此 layer 上叠加区域框选 Rectangle。
struct VideoPreview: NSViewRepresentable {
    @ObservedObject var model: PlayerModel

    func makeNSView(context: Context) -> PlayerHostingView {
        let view = PlayerHostingView()
        view.playerLayer.player = model.player
        return view
    }

    func updateNSView(_ nsView: PlayerHostingView, context: Context) {
        // player 重建（新 url）时同步到 layer
        if nsView.playerLayer.player !== model.player {
            nsView.playerLayer.player = model.player
        }
    }
}

/// 承载 AVPlayerLayer 的 NSView。
final class PlayerHostingView: NSView {
    let playerLayer = AVPlayerLayer()

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        setup()
    }

    required init?(coder: NSCoder) {
        super.init(coder: coder)
        setup()
    }

    private func setup() {
        wantsLayer = true
        playerLayer.videoGravity = .resizeAspect
        playerLayer.backgroundColor = NSColor.black.cgColor
        layer?.addSublayer(playerLayer)
    }

    override func layout() {
        super.layout()
        playerLayer.frame = bounds
    }
}

// MARK: - VideoControlsView

/// 播放控制条：时间标签 + 播放/暂停按钮 + 进度条。
struct VideoControlsView: View {
    @ObservedObject var model: PlayerModel

    var body: some View {
        HStack(spacing: 12) {
            // fallback 模式下禁用播放/暂停（mkv 不支持播放）
            Button(action: { model.togglePlay() }) {
                Image(systemName: model.isPlaying ? "pause.fill" : "play.fill")
                    .frame(width: 20)
            }
            .buttonStyle(.borderless)
            .keyboardShortcut(.space, modifiers: [])
            .disabled(model.loadFailed)

            Slider(
                value: Binding(
                    get: { Double(model.currentMs) },
                    set: { ms in
                        if model.loadFailed {
                            model.fallbackSeek(toMs: Int(ms))
                        } else {
                            model.seek(toMs: Int(ms))
                        }
                    }
                ),
                in: 0...Double(max(model.durationMs, 1))
            )

            Text("\(TimeFormatter.formatMs(model.currentMs)) / \(TimeFormatter.formatMs(model.durationMs))")
                .font(.system(.body, design: .monospaced))
                .foregroundStyle(.secondary)
                .frame(width: 200, alignment: .trailing)
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
    }
}
