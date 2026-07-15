# macOS GUI 模块设计

> `apps/macos/Sources/SubLiftMac/` — SwiftUI 壳、IPC 客户端、抽帧、编辑与导出。

## 模块职责

### Core

| 文件 | 职责 |
|---|---|
| `PipelineClient.swift` | 启动 Python IPC 子进程，通过 UDS 进行 4 字节长度前缀 + JSON 分帧的消息收发。 |
| `Messages.swift` | IPC 消息 Codable 定义，与 Python 端 `src/sublift/ipc/protocol.py` 字段对齐。 |
| `FrameSampler.swift` | `AVAssetReader` 按 fps 跳采样，将 `CVPixelBuffer` 编码为 JPEG；按扩展名路由 mkv 到 ffmpeg 兜底。 |
| `FfmpegFallback.swift` | 检测系统 ffmpeg/ffprobe、MJPEG stdout 流切帧（SOI/EOI marker）、mkv 抽帧与元数据探测。 |
| `PreviewLayout.swift` | 根据视频宽高比、左栏可用尺寸和控制区预留高度，为预览区计算受上限约束的高度。 |
| `SubtitleExtractor.swift` | 协调 `PipelineClient` + `FrameSampler`，完成启动 IPC → 流式抽帧 → finalize → 接收 entries 的端到端状态机；根据真实 progress 计算处理倍速。 |
| `ProcessingRate.swift` | 将已处理的视频时长除以实际处理耗时，格式化为相对实时的处理倍速（如 `4.0× 实时`）。 |
| `SubtitleEditor.swift` | 维护可编辑字幕列表，提供文本修改、合并、拆分与当前高亮节流。 |
| `SubtitleEntry.swift` | 带 `UUID` 的可变字幕条目模型，负责与 IPC 不可变 `SubtitleEntryData` 双向转换。 |
| `SrtFormatter.swift` | Swift 端 SRT 文本格式化，不经过 IPC 调 Python。 |
| `VisionTextDetector.swift` | 对代表帧执行 `VNRecognizeTextRequest`，返回文字候选框（归一化 rect + 文本 + 置信度）。 |
| `RegionSelectionModel.swift` | 候选框多选状态机，自动预选下部候选框并推算全宽 Y 带合并区域。 |
| `VideoCoordinateMapper.swift` | 像素/视图/Vision 归一化坐标换算与区域合并。 |
| `VideoMetadata.swift` | 异步加载视频文件名、大小、分辨率、时长、编码（AVFoundation + ffprobe 兜底 mkv）。 |

### UI

| 文件 | 职责 |
|---|---|
| `SubLiftMacApp.swift` | `@main` 入口、`ContentView` 组合、全局状态（videoURL / metadataLoader / extractor / editor）。 |
| `DropZone.swift` | 拖拽导入包装，`.dropDestination(for: URL.self)` 接收文件。 |
| `VideoPreview.swift` | `AVPlayerLayer` 视频预览 + 播放/暂停/seek 控制条 + PlayerModel。 |
| `RegionOverlay.swift` | 候选框叠加、候选列表、多选交互与合并区域预览。 |
| `SubtitleList.swift` | 字幕列表、文本编辑、合并/拆分/导出 SRT 按钮。 |
| `SettingsView.swift` | OCR 引擎选择（vision / mock），`@AppStorage("default_engine")` 持久化。 |
| `TimeFormatter.swift` | 毫秒 → `HH:MM:SS.mmm` 纯函数工具。 |

## IPC 协议

### 传输层

- **传输**：Unix Domain Socket，socket 路径由 Swift 端生成（`NSTemporaryDirectory() + UUID`）。
- **分帧**：4 字节大端长度前缀 + UTF-8 JSON body。
- **启动**：Swift 端 `Process.launchPath = .venv/bin/python`，参数 `["-m", "sublift.ipc.server", "--socket", sock_path, "--engine", engine]`。
- **关闭**：Swift 端 `stop()` 关闭 socket、terminate 子进程、unlink socket 文件。

### 消息类型

| 方向 | 消息 | 关键字段 |
|---|---|---|
| Swift → Python | `start_job` | `video_id`, `fps`, `engine`, `confidence_threshold`, `region_box: [x,y,w,h]?`, `duration_ms` |
| Swift → Python | `frame` | `video_id`, `ts_ms`, `jpeg_bytes: base64` |
| Swift → Python | `finalize` | `video_id`（通知帧流结束，Python 跑 Pipeline） |
| Swift → Python | `cancel_job` | `video_id` |
| Python → Swift | `progress` | `video_id`, `stage`, `pct`, `eta_ms` |
| Python → Swift | `entries` | `video_id`, `entries: [{start_ms, end_ms, text, confidence}]` |
| Python → Swift | `log` | `video_id`, `level`, `msg` |
| Python → Swift | `done` | `video_id`, `ok`, `error?` |
| 控制 | `hello` / `bye` / `error` | 握手与控制错误 |

### 安全保障

Python 端 `bridge.py` 设有多项上限，防止异常输入耗尽资源：

| 常量 | 含义 |
|---|---|
| `MAX_FRAMES` | 最多缓冲 60000 帧（约 3.3h @ 5fps） |
| `MAX_JPEG_BYTES` | 单帧 JPEG 上限 20MB |
| `MAX_IMAGE_PIXELS` | 单帧解码后像素上限 |
| `MAX_TOTAL_PIXELS` | 累计像素上限 |

JPEG 解码失败、base64 解码失败均返回 `done` / `log(error)`，不崩溃 server。

## 抽帧：打轴 vs 预览

### 打轴采样（统一后端，默认）

GUI 提取默认走 **path mode**：`start_job.video_path` 传入本地绝对路径，Python
`BridgeHandler` 使用 **`FfmpegExtractor`**（与 CLI / `run_benchmark` 同源）：

```
Swift start_job(video_path, fps, region_box, …)
  → Python FfmpegExtractor(fps)  raw RGB 均匀网格
  → Pipeline.feed / ocr_segment
  → progress + push_entry + entries(is_final=True)
```

- **不再**经 AVF PTS 跳采样 + JPEG q=0.85 推帧（避免与验收 F1 漂移 ~10pp）。
- 无 `video_path` 时仍可走 legacy **frame mode**（Swift 推 JPEG）供调试。
- 提取栏的处理倍速由 Swift 根据 path mode 的真实进度与本地计时计算：`(processed_frames / sample_fps) / elapsed_seconds`。它表示处理吞吐量而非播放速度；开始 0.25 秒内不显示，以避免计时粒度造成的跳变。

### 预览 / 选区用帧（仍可 AVF）

代表帧 Vision 候选框、播放预览继续用 AVFoundation；与打轴采样解耦。

### 左栏自适应布局

左栏将预览、播放控制、元数据、区域候选和提取栏放在单一纵向布局中。预览高度取
`min(按源视频比例拟合的高度, 可用高度 - 240pt 控制区预留, 420pt)`；因此缩小窗口时，
预览先收缩而不会吞掉下方交互。提取栏把按钮/引擎与采样/状态拆为两行，状态文本限制
为单行截断，避免窄栏频繁换行。

### 历史：Swift 侧 AVF / ffmpeg 兜底（frame mode / 预览）

```
AVURLAsset → AVAssetReader → PTS 跳采样 → JPEG q=0.85
.mkv 等：系统 ffmpeg MJPEG 流（FfmpegFrameSampler）
```

- `FfmpegDetector.whichFfmpeg()` 检测系统 ffmpeg，缺失弹窗引导 `brew install ffmpeg`。

## 字幕编辑模型

`SubtitleEditor`（`@MainActor ObservableObject`）维护可编辑字幕列表：

| 操作 | 行为 |
|---|---|
| `load(_ entries:)` | 从 IPC `entries` 拷贝为带 UUID 的可编辑模型 |
| `updateText(at:id, text:)` | 修改单条文本 |
| `merge(at:index)` | 与下一条合并：start = 当前.start，end = 下一条.end，text = "当前 下一条"，confidence = max |
| `split(at:index)` | 在中点 split：前段保留原 text，后段 text 置空 |
| `updateCurrent(atMs:)` | 根据播放时间更新 `currentId`，内部节流避免频繁刷新 |
| `exportEntries()` | 返回 `[SubtitleEntryData]` 供 `SrtFormatter` 使用 |

编辑后的导出不走 IPC，直接由 Swift 端 `SrtFormatter.format(entries:)` 格式化并写文件。

## Vision 字幕区域检测

### 检测流程

```
代表帧 CGImage
  → VNRecognizeTextRequest(recognitionLanguages: ["zh-Hans", "en-US"])
  → VNImageRequestHandler.performRequests
  → observations.topCandidates(1)
  → 候选框数组（归一化 rect + text + confidence）
```

### 多选与合并

- `RegionSelectionModel` 自动预选位于画面下部的候选框（字幕常见位置）。
- 用户可点击候选列表增/减选择。
- `VideoCoordinateMapper.mergedFullWidthRegion(selected:in:)` 将选中框合并为：
  - `x = 0, width = video_width`
  - `y = min(selected.y) - padding, height = max(selected.maxY) - min(selected.y) + 2*padding`
- 合并后的 `region_box: [x,y,w,h]` 通过 `start_job` 传给 Python，`bridge.py` 据此选择 `FixedRegionDetector`；无选择时回退 `BottomCropDetector`。

## SRT 导出

`SrtFormatter` 为纯函数 enum，输入 `[SubtitleEntryData]`：

```
1
00:00:01,000 --> 00:00:05,000
字幕文本

2
...
```

- 时间码格式 `HH:MM:SS,mmm`，小时可超 24 不回绕。
- 条目间空行，末尾保留换行。
- 非法时间戳（负数或 endMs < startMs）抛出 `SrtFormatError`。

导出由 `SubtitleList` header 的「导出 SRT」按钮触发，`SubLiftMacApp` 中通过 `NSSavePanel` 选路径并 UTF-8 写文件。

## 已知约束

- **仅开发者构建运行**：Phase 2 不做独立 `.app` 与公证，GUI 通过 `swift run SubLiftMac` 启动。
- **mkv 依赖系统 ffmpeg**：未安装时 UI 禁用 mkv 拖入并弹窗引导。
- **首条识别结果 ≤10s 目标**：当前实现为流式抽帧 + 批量 OCR（feat-018 务实方案），真增量需后续改造 Pipeline 为滑动窗口模型。
- **时间码拖动调整未实现**：编辑功能目前仅支持文本修改、合并、拆分，时间码手动调整留待后续。
