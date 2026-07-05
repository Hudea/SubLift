import AVFoundation
import Combine
import SwiftUI

// MARK: - PlayerModel

/// AVPlayer 状态桥接器：把 AVPlayer 的播放状态、当前时间暴露为 @Published。
final class PlayerModel: ObservableObject {
    @Published private(set) var currentMs: Int = 0
    @Published private(set) var durationMs: Int = 0
    @Published private(set) var isPlaying: Bool = false
    @Published private(set) var player: AVPlayer?

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
            guard item.status == .readyToPlay else { return }
            let secs = CMTimeGetSeconds(item.duration)
            DispatchQueue.main.async {
                if secs.isFinite {
                    self?.durationMs = Int(secs * 1000)
                }
            }
        }
    }

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
            Button(action: { model.togglePlay() }) {
                Image(systemName: model.isPlaying ? "pause.fill" : "play.fill")
                    .frame(width: 20)
            }
            .buttonStyle(.borderless)
            .keyboardShortcut(.space, modifiers: [])

            Slider(
                value: Binding(
                    get: { Double(model.currentMs) },
                    set: { model.seek(toMs: Int($0)) }
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
