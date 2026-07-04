# Phase 2 设计与执行计划

> 本文件是 Phase 2 的设计源头与执行手册。任务跟踪见 `docs/phases/phase2.json`,
> 项目级功能块见 `feature-list.json` 的 `phases[phase2]` 块。

## 1. 目标与范围

### 1.1 Phase 2 目标

交付**可分发的 macOS `.app`**:从拖入视频到看到第一条识别结果 ≤ 10 秒,
字幕条目可在 UI 中编辑并重新导出为 SRT;Python 算法核心零修改,
通过 UDS + MsgPack 与 SwiftUI 壳通信。

```
[SwiftUI 主进程] ──UDS+MsgPack──> [Python 子进程]
   (UI / 抽帧 / 编辑)              (Phase 1 Pipeline + Vision)
```

### 1.2 在范围内

- `apps/macos/` 全新 SwiftUI 工程(与 `src/sublift/` Python 包双工程并列)
- **Swift 端**
  - 主窗口(三栏:左预览 / 中时间轴 / 右编辑)
  - 拖拽导入(DropZone)+ 视频元数据解析
  - AVPlayer 视频预览
  - AVFoundation 抽帧(mp4/mov/H.264/HEVC 优先,VideoToolbox 硬解)
  - 系统 ffmpeg 兜底(mkv 等 AVFoundation 不支持的格式;缺失则弹窗引导)
  - 字幕时间轴 + 列表 + 编辑(双击改文本、拖动改 start_ms/end_ms、合并/拆分)
  - 区域框选(鼠标在画面画 Rectangle)→ 重新跑流水线
  - SettingsView(vision / mock 引擎选择,UserDefaults 持久化)
  - SRT 导出(`NSSavePanel`)
- **Python 端**
  - `src/sublift/ipc/` 新模块:UDS server + MsgPack 协议 + bridge 把 Phase 1 Pipeline 包成 IPC handler
  - **Phase 1 核心模块(`pipeline/` / `extractor/` / `detector/` / `ocr/` / `export/` / `models.py` / `config.py`)零修改**
- **打包 / 分发**
  - SwiftPM `.build/release/SubLiftMac.app`
  - embedded Python.framework + sublift 包
  - `codesign --deep --options=runtime` + `xcrun notarytool` + `xcrun stapler`
  - Developer ID 公证

### 1.3 不在范围内(MVP 后置 / 后续 Phase)

- ASS / VTT 完整实现(CLI 与 GUI 同)
- PaddleOCR 第二引擎(接口预留)
- 引擎自动路由 / 对照模式(F10 / F13)
- 配置文件 `sublift.toml`
- 字幕翻译 / 实时直播 / 软字幕提取
- Phase 3 跨平台(Windows / Linux)
- App Store 上架(仅 Developer ID 直链分发;App Store 阶段再开 App Sandbox)
- iCloud / 账户系统
- 区域框选多边形(仅 MVP 单矩形)

### 1.4 验收门

#### 功能
- [ ] `.app` 通过 Developer ID + notarization + Gatekeeper 实测(feat-025 spike)
- [ ] 1080p / 5fps / mp4 / Vision 引擎,拖入到首条识别结果 ≤ 10s
- [ ] mkv 通过系统 ffmpeg 兜底跑通
- [ ] 字幕条目双击改文本、拖动改 start_ms/end_ms、合并/拆分生效
- [ ] re-export SRT 与编辑后一致
- [ ] SettingsView 切换 vision/mock,重启后保持
- [ ] 区域框选新区域后重新跑流水线,列表更新

#### 工具链
- [ ] `uv run pytest` / `uv run ruff check .` / `uv run mypy src tests` 全绿(Python 端无回归)
- [ ] `xcodebuild test` 全绿(Swift 端至少 IPC 单测 + UI smoke)
- [ ] `./init.sh` 退出 0
- [ ] `apps/macos/scripts/test_gui_e2e.sh` 端到端冒烟通过

## 2. 模块布局

```
SubLift/
  apps/                                  # NEW: macOS 端工程根
    macos/                              # NEW: SwiftUI 壳工程
      Package.swift                      # SwiftPM 入口(macOS 13+)
      project.yml                        # xcodegen 配置(spike 后定)
      Sources/SubLiftMac/
        App/
          SubLiftMacApp.swift            # @main 入口
          AppDelegate.swift
        UI/
          MainWindow.swift               # 三栏主窗口
          DropZone.swift                 # 拖拽导入
          VideoPreview.swift             # AVPlayerLayer 预览
          RegionPicker.swift             # 区域框选
          SubtitleList.swift             # 字幕列表 + 编辑
          SettingsView.swift             # 引擎选择
        Core/
          PipelineClient.swift           # UDS + MsgPack 客户端
          FrameSampler.swift             # AVFoundation 抽帧
          FfmpegFallback.swift           # 系统 ffmpeg 兜底
          RegionDetector.swift           # 框选 → Region
          SubtitleEditor.swift           # 编辑模型
        Resources/
          Info.plist                     # 不开 sandbox + UDS 权限
          entitlements.plist
      Tests/                             # XCTest
        IPCTests.swift
        UISmokeTests.swift
      scripts/
        test_gui_e2e.sh                  # 端到端冒烟
  src/sublift/                           # 现有(基本不动)
    ipc/                                 # NEW: Python 端 IPC server
      __init__.py
      server.py                          # UDS server 入口
      protocol.py                        # MsgPack schema 常量与编解码
      bridge.py                          # 把 Phase 1 Pipeline 包成 IPC handler
    # 以下模块 Phase 2 零修改:
    pipeline/  extractor/  detector/  ocr/  export/
    models.py  config.py
```

**关键边界**:`pipeline/` / `extractor/` / `detector/` / `ocr/` / `export/` / `models.py` / `config.py` 在 Phase 2 **零修改**。新代码只进 `apps/macos/` 与 `src/sublift/ipc/`。

## 3. 数据流

```
[用户拖入 mp4]
   ↓
[DropZone → AppDelegate.handleDrop(url)]
   ↓
[FrameSampler.start(asset, fps=5)]
   ├─ AVURLAsset.loadTracks(.video) 成功 → 走 AVFoundation
   │     CVPixelBuffer → CGImage → JPEG (q=85)
   │     ↓ "ts_ms:u64, jpeg_bytes" 帧流
   └─ 失败(.mkv 或 -11828) → 走 FfmpegFallback
        检测系统 ffmpeg(缺失弹窗引导 brew install)
        spawn ffmpeg -i input -vf fps=5 -f image2pipe -vcodec mjpeg -
        ↓
[PipelineClient.send_frame(ts_ms, jpeg, region)]
   ↓
[UDS socket write msgpack]
   ↓
─────────────────────────────────────
[Python ipc.server.receive()]
   ↓
[bridge.handle_frame()] → PIL.Image.open(BytesIO(jpeg))
   ↓
[sublift.pipeline.Pipeline.run(stream)] ←  Phase 1 复用
   ↓
[ipc.server.send_entries(entries: list[SubtitleEntry])]
   ↓
[PipelineClient.on_entries] → @Published entries
   ↓
[SubtitleList 显示 + 编辑](双击改文本 / 拖动改时间 / 合并 / 拆分)
   ↓
[NSSavePanel] → 调用 SrtExporter.export()
```

## 4. IPC 协议(UDS + MsgPack)

### 4.1 消息类型

| 方向 | 消息 | 字段 |
|---|---|---|
| Swift → Python | `start_job` | `video_id, fps, region_box, engine, confidence_threshold` |
| Swift → Python | `frame` | `video_id, ts_ms, jpeg_bytes, region_box?`(可选覆盖) |
| Swift → Python | `cancel_job` | `video_id` |
| Python → Swift | `progress` | `video_id, stage, pct, eta_ms` |
| Python → Swift | `entries` | `video_id, entries: [{start_ms, end_ms, text, confidence}]` |
| Python → Swift | `log` | `video_id, level, msg` |
| Python → Swift | `done` | `video_id, ok, error?` |

### 4.2 协议实现

- **传输层**:Unix Domain Socket(`/tmp/sublift-<pid>.sock`)
- **序列化**:JSON(按 ADR-0007c/d,MsgPack 评估后撤销,详见 HURDLES)
- **消息分帧**:4 字节大端长度前缀 + UTF-8 JSON body
- **Server 端**:`asyncio.start_unix_server` + `json.loads`
- **Client 端**:`DispatchSourceRead` + `JSONSerialization`(feat-014 已实现)
- **消息 schema**:feat-015 在 `src/sublift/ipc/protocol.py`(Python) + `apps/macos/.../Core/Messages.swift`(Swift Codable)集中定义 7 类消息
- **错误处理**:socket 断开 → 弹窗报错,用户重新拖入(MVP 不做自动重启)

### 4.3 进程启动

- Swift 启动时:`Process().launchPath = python_bin, arguments = ["-m", "sublift.ipc.server", "--socket", sock_path]`
- Python 端:UDS 监听,循环接收,处理,归还结果
- 子进程 crash 监控:Swift 端持有子进程 PID,`Process.terminationHandler` 触发 UI 错误提示

## 5. 抽帧与 ffmpeg 兜底

### 5.1 AVFoundation 路径(主路径)

- `AVURLAsset(url: url).loadTracks(withMediaType: .video)`
- `AVAssetReader` + `AVAssetReaderTrackOutput`,`kCVPixelBufferPixelFormatTypeKey` = `kCVPixelFormatType_32BGRA`
- 按 `fps=5` 跳采样(每帧 PTS ≥ 200ms)
- `CVPixelBuffer` → `CGImage`(`CVPixelBufferGetCGImage`)→ JPEG q=85
- **输出**:`(ts_ms, jpeg_bytes)` 流

### 5.2 ffmpeg 兜底路径

- 启动时检测:`Process.run("/usr/bin/env", ["which", "ffmpeg"])` + 缓存
- 缺失:弹窗 `brew install ffmpeg` 引导,UI 禁用 mkv 拖入
- 存在:`spawn ffmpeg -i input -vf fps=5 -f image2pipe -vcodec mjpeg -`
- 读 stdout MJPEG 流,组装 JPEG 字节送 IPC

### 5.3 抽帧抽象

Phase 1 `Extractor` Protocol 是「视频文件 → 帧迭代器」。Phase 2 引入新实现:

- Swift 端 `FrameSampler`(AVFoundation / ffmpeg)输出 JPEG 字节流
- Python 端 IPC 接收 JPEG,重建为 `PIL.Image`,用 `Frame(timestamp_ms, image)` 注入 Pipeline
- **Pipeline 接入**:按 ADR-0007a,在 `Pipeline` 上新增 `run_frames(frames: Iterator[Frame]) -> list[SubtitleEntry]`,`run(video_path)` 重构为内部调 `run_frames`。`bridge.py` 调 `run_frames`。Phase 1 CLI 行为不变。

## 6. 任务拆分

> 任务粒度按 ADR-0004:粗任务承载主验收,subtasks 承载细节。

### 6.1 任务列表

| id | 任务 | 依赖 | 验收 |
|---|---|---|---|
| **feat-012** | AVFoundation 抽帧 spike | — | 6 个测试素材(mp4-h264, mp4-hevc-8bit, mp4-hevc-10bit, mov-h264, mov-hevc, mkv-h264)跑通;mkv 触发 -11828 时切 ffmpeg;spike 报告 `docs/spikes/avf-phase2.md` |
| **feat-013** | 双工程结构 + Package.swift | feat-012 | `swift build` 编译空 SwiftUI app 成功 |
| **feat-014** | Python UDS service 启动骨架 | feat-013 | Swift 启动 Python 子进程,hello 消息往返 |
| **feat-015** | IPC 协议 + MsgPack 序列化 | feat-014 | `start_job` / `frame` / `entries` / `progress` / `done` 五类消息序列化通过单测 |
| **feat-016** | Python bridge 包装 Phase 1 Pipeline | feat-015 | Swift 送 JPEG 帧流 → Python 端 Pipeline 跑通,收到 `entries` 消息 |
| **feat-017** | 视频预览(AVPlayer)+ 当前帧时间显示 | feat-013 | 播放本地视频,显示当前帧时间戳(ms) |
| **feat-018** | AVFoundation 抽帧 + IPC 帧流端到端 | feat-012, feat-016, feat-017 | 5 fps 抽 1080p mp4 5s,Python 端 Pipeline 跑通,首条结果返回 UI;首条识别结果 ≤ 10s benchmark 记录 |
| **feat-019** | mkv 兜底 + 系统 ffmpeg 检测 | feat-018 | 拖入 mkv → 检测 ffmpeg → 抽帧 → IPC 跑通;缺失 ffmpeg 时弹窗引导 `brew install` |
| **feat-020** | 拖拽导入(DropZone)+ 视频元数据解析 | feat-013, feat-018 | 拖入 mp4/mkv → 显示文件名/分辨率/时长/编码 |
| **feat-021** | 字幕时间轴 + 列表 + 编辑(双击改文本/拖动改时间/合并/拆分) | feat-018 | 编辑后 re-export SRT 与编辑一致 |
| **feat-022** | 区域框选(鼠标画 Rectangle)→ 重新跑流水线 | feat-021 | 框选新区域 → IPC 端用新 Region → 列表更新 |
| **feat-023** | 引擎选择(SettingsView + UserDefaults) | feat-016 | 切换 vision/mock,持久化,重启后保持 |
| **feat-024** | SRT 导出(`NSSavePanel`) | feat-021 | 导出 SRT 可在 IINA/VLC 正常加载 |
| **feat-025** | `.app` 打包 + Developer ID 公证 spike | feat-019, feat-024 | 签名 + notarytool 上传 + Gatekeeper 通过;spike 报告 `docs/spikes/notarization-phase2.md` |
| **feat-026** | 文档收尾(ARCHITECTURE/REQUIREMENTS/DECISIONS/README) | feat-025 | 反映新工程,ADR-0005/0006 落定 |

**合计 15 个粗任务(含 2 个 spike 报告)。**

### 6.2 执行顺序

```
feat-012 (AVF spike) ─┐
                       ├─ feat-013 (双工程 + SwiftPM)
                       │       ├─ feat-014 (UDS 骨架)
                       │       │      └─ feat-015 (IPC + MsgPack)
                       │       │             └─ feat-016 (Python bridge)
                       │       │                    └─ feat-023 (引擎选择)
                       │       ├─ feat-017 (AVPlayer 预览)
                       │       └─ feat-020 (DropZone)
                       │              │
                       │              └─ feat-018 (AVF + IPC 帧流 E2E) ─┐
                       │                                              ├─ feat-019 (mkv 兜底)
                       │                                              ├─ feat-021 (编辑)
                       │                                              │      ├─ feat-022 (区域框选)
                       │                                              │      └─ feat-024 (SRT 导出)
                       │                                              │
                       │                                              └─ feat-025 (公证 spike)
                       │                                                     └─ feat-026 (文档)
```

### 6.3 里程碑切分

- **Phase 2a 基础架构**(feat-012~016):双工程、UDS + MsgPack、Python bridge 把 Phase 1 Pipeline 接入
- **Phase 2b 预览 + 抽帧**(feat-017~020):AVPlayer 预览、AVFoundation 抽帧、DropZone、mkv 兜底
- **Phase 2c 编辑 + 导出**(feat-021~024):字幕编辑、区域框选、引擎选择、SRT 导出
- **Phase 2d 收尾 + 公证**(feat-025~026):公证 spike、文档收尾、ADR 落定

## 7. 关键技术约束

| 约束 | 说明 | 缓解 |
|---|---|---|
| 公证可行性 | embedded Python + hardened runtime 在 macOS 13+ 公开案例有限 | feat-012 / feat-025 双 spike 验证;失败则降级 PySide6 |
| ffmpeg 缺失体验 | mkv 必须有 ffmpeg 才能处理 | 启动时 `which ffmpeg` 检测 + 弹窗引导;UI 禁用 mkv 拖入 |
| 10 秒首帧反馈 | Python 进程冷启动 + Vision 初始化 + ffmpeg/AVF 抽帧流水线 | Python 进程常驻 + 预热 Vision + 并行抽帧与 OCR;降级 fps=3 |
| JPEG q=85 对 OCR 精度 | 字幕边缘轻微模糊可能影响识别 | feat-018 spike:q=85 vs PNG 对 Vision CER 对比 |
| App Sandbox 关闭 | 法务 / App Store 上架 | MVP 不上 App Store,App Store 阶段再开 sandbox + 重做 IPC |
| Apple Developer ID | $99/年付费账号 | CI 需 `APPLE_CERTIFICATE` / `APPLE_API_KEY` 等 secret 流转 |
| macOS 13/14 行为差异 | AVFoundation 容器解析可能与 macOS 15 不同 | feat-012 spike 覆盖 13/14/15 三版本 |
| 区域框选 UI | SwiftUI 自定义绘图较复杂 | MVP 简版(单矩形 DragGesture);多边形 v2 |

## 8. 阶段验收门

### 8.1 Phase 2a(基础架构,feat-012~016)

- [ ] `swift build` 成功
- [ ] Python UDS 子进程可被 Swift launch 并 hello/bye
- [ ] 5 类消息序列化 Python + Swift 双向单测通过
- [ ] Python bridge 跑通 Phase 1 Pipeline
- [ ] Python 端 `uv run pytest` / `uv run ruff check .` / `uv run mypy src tests` 全绿

### 8.2 Phase 2b(预览 + 抽帧,feat-017~020)

- [ ] AVPlayer 播放本地视频,显示当前帧时间
- [ ] 5 fps 抽 1080p mp4 5s,IPC 帧流 Python 端收到
- [ ] mkv 走 ffmpeg 兜底
- [ ] DropZone 显示元数据

### 8.3 Phase 2c(编辑 + 导出,feat-021~024)

- [ ] 字幕双击改文本、拖动改时间、合并/拆分生效
- [ ] 区域框选新区域重新跑流水线
- [ ] SettingsView 引擎切换持久化
- [ ] SRT 导出可用 IINA/VLC 加载

### 8.4 Phase 2d(收尾 + 公证,feat-025~026)

- [ ] `.app` 通过 Developer ID + notarytool + Gatekeeper
- [ ] `xcodebuild test` 全绿
- [ ] `apps/macos/scripts/test_gui_e2e.sh` 通过
- [ ] `docs/ARCHITECTURE.md` 加 macOS 端工程章节
- [ ] `docs/REQUIREMENTS.md` §3.4 状态推进
- [ ] `docs/DECISIONS.md` 增 ADR-0005(GUI 架构)、ADR-0006(抽帧双方案)
- [ ] `README.md` 加 GUI 截图 + `.app` 安装段
- [ ] `docs/design/macos-gui.md` GUI 模块设计(与 `phase1.md` design 平齐)

## 9. 风险与权衡

| 风险 | 影响 | 缓解 | 状态 |
|---|---|---|---|
| embedded Python + 公证不可行 | Phase 2 路线需重定 | feat-012/025 spike 早验证 | 待 spike |
| ffmpeg 缺失需用户安装 | mkv 用户体验差 | 启动时检测 + 弹窗引导 | MVP 取舍 |
| 10 秒首帧反馈难达 | 验收不通过 | Python 常驻 + 预热 + 降级 fps | 待 feat-018 benchmark |
| 区域框选 UI 难用 | F23 体验差 | MVP 简版(单矩形) | MVP 取舍 |
| SwiftUI 与 Python 类型同步 | IPC schema 双写易错 | MsgPack 集中定义 + 双向手写序列化 | feat-015 设计 |
| 跨平台演化(Phase 3) | UI 壳需可换 | UDS 边界是天然 API;Tauri 可平替 | 已留路 |
| App Store 上架 | 需开 sandbox + 改 IPC | Phase 2 后置 | 已声明 |
