# SubLift 架构设计

## 1. 设计原则

- **模块化、可插拔、低耦合高内聚**：各能力模块定义 Protocol，默认实现可替换
- **平台 API 隔离**：平台特定 API（Apple Vision）只能出现在 `ocr/vision.py`，核心层只依赖 Protocol
- **串联层不实现单一能力**：pipeline / export 组合能力模块，处理流程逻辑

## 2. 分层架构

| 层 | 模块 | 职责 |
|---|---|---|
| 能力模块 | `extractor` / `detector` / `ocr` | 单一能力，Protocol + 默认实现，可插拔 |
| 串联层 | `pipeline` / `export` | 组合能力模块，处理时间轴 / 去重 / 导出 |
| 入口层 | `cli` | 参数解析，调用 pipeline |
| 共享 | `models` / `config` | 核心数据模型与默认配置 |

## 3. 模块布局

```
src/sublift/
  __init__.py
  __main__.py              # python -m sublift 入口
  cli.py                   # CLI 入口（argparse）
  models.py                # 核心数据模型（frozen dataclass）
  config.py                # 默认配置 + SignatureConfig + ChangePointConfig
  pipeline/                # 串联层：打轴与编排
    signature.py           # 帧签名（前景占比 + dHash 双信号）
    changepoint.py         # 三态状态机（EMPTY⇄STABLE→STABLE'）
    timeline.py            # 时间轴构建（消费事件流）
    dedupe.py              # 去重合并（3-pass 纯函数）
    core.py                # Pipeline 类（端到端编排）
  detector/                # 能力模块：字幕区域检测
    base.py                # Detector Protocol
    bottom_crop.py         # 按比例裁剪下部（默认 30%）
    fixed_region.py        # 手动指定区域
  extractor/               # 能力模块：帧采样
    base.py                # Extractor Protocol
    ffmpeg_extractor.py    # ffmpeg/ffprobe subprocess 实现
  ocr/                     # 能力模块：OCR（可插拔多引擎）
    base.py                # OcrEngine Protocol
    mock.py                # MockOcrEngine（测试用）
    vision.py              # VisionOcrEngine（Apple Vision，PyObjC）
  export/                  # 串联层：字幕导出
    base.py                # Exporter Protocol（format + export 双方法）
    srt.py                 # SRT 实现
    ass.py                 # ASS 占位
    vtt.py                 # VTT 占位
```

## 4. 数据流

```
video
  └─[extractor]→ Frame(t, image)
      └─[detector, 首帧一次性]→ Region(box)
          └─[crop]→ 字幕带图像
              └─[signature]→ FrameSignature(fg_ratio, dhash)
                  └─[changepoint]→ StateEvent(IN/OUT/CHANGE)
                      └─[timeline + 段内采样]→ SegmentEvent(start_ms, end_ms, frames)
                          └─[ocr, 每段最多 4 个代表帧]→ tuple[OcrLine,...]
                              └─[SubtitleProfile 选行 + 跨帧共识]→ text
                                  └─[dedupe]→ list[SubtitleEntry]
                                      └─[export]→ SRT 文件
```

详细设计见各模块文档：

- [pipeline 设计](design/pipeline.md) — 帧签名、状态机、时间轴、去重、编排
- [ocr 设计](design/ocr.md) — Protocol、Vision 实现、Mock、跨平台演进
- [extractor 设计](design/extractor.md) — Protocol、ffmpeg 实现、流式采样

## 5. 核心数据模型

`src/sublift/models.py` — 全部 `@dataclass(frozen=True)`，跨模块共享，避免循环依赖。

| 模型 | 字段 | 用途 |
|---|---|---|
| `BoundingBox` | `x, y, width, height: int` | 矩形区域，绝对像素坐标 |
| `Region` | `box: BoundingBox` | 字幕区域 |
| `Frame` | `timestamp_ms: int, image: PIL.Image` | 视频帧，附带时间戳 |
| `OcrLine` | `text, confidence, box: BoundingBox` | 单行 OCR；box 相对 recognize 输入图、像素左上 |
| `OcrResult` | `text, confidence, lines: tuple[OcrLine,...]` | OCR 结果；text/conf 为兼容汇总（`\\n` + 均值），行级以 lines 为准 |
| `SubtitleProfile` | `script, center_x/y, height, y_min/y_max` | 字幕轨画像（feat-034b）；几何相对 region crop，供行级选择 |
| `SubtitleEntry` | `start_ms, end_ms: int, text: str` | 字幕条目，pipeline 产出，export 消费 |

## 6. 抽象接口

四个 Protocol 均 `@runtime_checkable`，支持 `isinstance` 静态判定。

| Protocol | 方法 | 输入 → 输出 |
|---|---|---|
| `Extractor` | `extract` | `Path → Iterator[Frame]` |
| `Detector` | `detect` | `Frame → Region` |
| `OcrEngine` | `recognize` | `PIL.Image → OcrResult` |
| `Exporter` | `format` / `export` | `list[SubtitleEntry] → str` / `(entries, Path) → None` |

## 7. 技术栈

| 依赖 | 用途 | 性质 |
|---|---|---|
| Python 3.12+ | 语言 | 基线 |
| uv | 包管理 | 环境 |
| ffmpeg / ffprobe | 抽帧 + 探测 | 系统依赖（subprocess 调用） |
| Pillow | 图像中立表示 | 必需 |
| NumPy | 数组运算 | 必需 |
| opencv-python-headless | 自适应二值化、形态学、SSIM | 必需 |
| pyobjc-framework-Vision | Apple Vision OCR | macOS 可选 |
| pyobjc-framework-Quartz | CGImage/CGDataProvider | macOS 可选 |
| pytest / ruff / mypy | 测试 / lint / 类型检查 | dev |

## 8. 配置

`src/sublift/config.py` — `Config` dataclass，含 `SignatureConfig` 与 `ChangePointConfig` 嵌套配置。

| 参数 | 默认值 | 说明 |
|---|---|---|
| `sample_fps` | 5.0 | 帧采样率 |
| `region_bottom_ratio` | 0.3 | 字幕区域裁剪比例（下部 30%） |
| `confidence_threshold` | 0.5 | OCR 置信度阈值（行级选择下为高 conf 门） |
| `subtitle_script` | auto | 无显式 profile 时的文字系统；CLI/benchmark 可固定 cjk/latin |
| `enable_line_select` | True | feat-034：按 SubtitleProfile 选行 + 多帧共识 |
| `low_conf_threshold` | 0.28 | 低置信多帧稳定放行下限 |
| `ocr_consensus_frames` | 4 | 段内最多 OCR 代表帧数 |
| `merge_gap_ms` | 1000 | 去重合并间隔阈值 |
| `min_duration_ms` | 500 | 最小字幕时长 |

完整字段见 `src/sublift/config.py`，各参数说明见 [pipeline 设计](design/pipeline.md)。

## 9. 当前质量水位与已知限制

固定 GT（Zootopia clip、1080p、5fps、统一 diagnostic 口径）的 Phase 3 最终结果：timing recall **96.6%**、precision **98.8%**、F1 **97.7%**；CER macro **3.2%**、micro **2.4%**、字符准确率 **97.6%**；usable subtitle recall **92.0%**；noise/empty 均为 **0**；处理速度 **21.0×** 实时。报告见 `debug/benchmark-reports/feat034_p1_fix2/`。

上述结果是固定回归锚点，不是跨片源泛化承诺。当前限制：

- **GT 多样性不足**：主要依赖 Zootopia 中文字幕片段，英文、中英混排、不同字幕位置和不同片源尚未形成固定 GT。
- **混排边界风险**：显式 CJK 画像的边界 cleanup 仍可能误删 `NPD动物警局`、`苹果的iPhone` 一类无空格英文，默认 `auto` 可规避部分风险。
- **打轴 residual**：极短字幕和 merged cluster（如 #15）仍可能漏检或合并。
- **长视频 GUI 手工验收暂缓**：自动 4K 内存、首条反馈、取消与重启审计已通过，但尚未用 ≥10 分钟非 Zootopia 视频验证完整交互体验。
- **ASS/VTT 仅占位**：当前产品导出 SRT。

## 10. Phase 2 macOS GUI 架构

Phase 2 在 Phase 1 CLI 核心之上叠加 macOS GUI，采用 **SwiftUI 壳 + Python UDS 子进程** 的双工程布局（ADR-0005）。

### 10.1 双工程布局

```
SubLift/
  src/sublift/            # Python 核心（Phase 3 已扩展增量与行级 OCR）
    ipc/                  # 新增：UDS server / protocol / bridge
    pipeline/             # 串联层
    extractor/            # 能力模块：帧采样
    detector/             # 能力模块：字幕区域检测
    ocr/                  # 能力模块：OCR
    export/               # 串联层：字幕导出
  apps/macos/             # 新增：SwiftUI 壳工程
    Package.swift
    Sources/SubLiftMac/
      App/
        SubLiftMacApp.swift
      Core/               # 业务逻辑与 IPC 客户端
      UI/                 # SwiftUI 视图
    Tests/SubLiftMacTests/
```

### 10.2 进程间通信

| 项 | 说明 |
|---|---|
| 传输 | Unix Domain Socket（本地 socket 文件） |
| 分帧 | 4 字节大端长度前缀 + UTF-8 JSON body |
| Swift 端 | `PipelineClient` 启动 `python -m sublift.ipc.server --socket <path>`，通过 BSD socket 收发 |
| Python 端 | `asyncio.start_unix_server` + `src/sublift/ipc/bridge.py` 处理业务消息 |
| 消息类型 | `start_job` / `frame` / `finalize` / `cancel_job` / `progress` / `push_entry` / `entries` / `log` / `done` |

### 10.3 Swift 端分层

| 层 | 文件示例 | 职责 |
|---|---|---|
| App | `SubLiftMacApp.swift` | `@main` 入口、`ContentView` 组合、状态管理 |
| Core | `PipelineClient.swift`、`FrameSampler.swift`、`SubtitleExtractor.swift`、`SubtitleEditor.swift`、`SrtFormatter.swift` | IPC 客户端、抽帧、提取协调、编辑模型、导出格式化 |
| UI | `VideoPreview.swift`、`SubtitleList.swift`、`RegionOverlay.swift`、`SettingsView.swift` | 视频预览、字幕列表、区域候选框、设置 |

### 10.4 GUI 数据流

```
用户拖入视频
  ↓
DropZone → VideoMetadata.load(url)
  ↓
AVFoundation ──→ 预览与 Vision 候选框（不参与默认打轴采样）
  ↓
PipelineClient.start_job(video_path, region_box, SubtitleProfile)
  ↓ UDS + JSON
ipc.bridge.BridgeHandler path mode
  ↓
Python FfmpegExtractor ──逐帧推进──→ Pipeline.feed(frame)
  ↓
[段闭合时触发] push_entry 增量推送 → SubtitleEditor 实时追加展示
  ↓
[全片结束] entries 最终批量合并推送 → SubtitleEditor.load(entries)
  ↓
SubtitleList 显示 / 编辑 / SrtFormatter.format() → NSSavePanel 写文件
```

默认 GUI、CLI 与 benchmark 共享 Python `FfmpegExtractor` 的像素和时间戳序列（ADR-0010）。Swift 推 JPEG 的 frame mode 仅保留为兼容与调试路径。

### 10.5 关键约束

- **历史 Phase 2 约束**：Phase 2 尽量不改 Python 核心；Phase 3 已明确解除该约束，以实现增量 pipeline、统一抽帧和 OCR 行级选择。
- **平台 API 隔离**：Apple Vision 在 GUI 端仅用于字幕区域候选框检测（`VisionTextDetector.swift`），OCR 仍由 Python 端 `ocr/vision.py` 执行。
- **无分发包**：Phase 2 不做独立 `.app` 打包与 Apple 公证（ADR-0009），GUI 通过 `swift run SubLiftMac` 在开发者环境运行。

## 11. Phase 3 优化架构

### 11.1 Benchmark 驱动

`scripts/run_benchmark_manifest.py` 以 manifest 固定视频、GT、区域与配置，输出 summary、agent JSON、GT CSV 和 detection CSV。所有质量结论先看同一 GT 总表，再拆 failure clusters，最后定位 cue 与根因；`docs/benchmarks/baseline.md` 保存关键水位导航。

### 11.2 增量处理与取消

`Pipeline` 拆为 `feed(frame)`、`ocr_segment(segment)` 与 `finalize()`：抽帧时持续推进打轴，段闭合即 OCR 并通过 IPC `push_entry` 推送，结束时再做最终去重。GUI path mode 的 worker 在线程中读取 `FfmpegExtractor`，进度消息携带实际帧数、总帧估计和 `processing/finalizing` 阶段。取消同时设置持久化 cancellation event 并终止 ffmpeg 子进程；worker 在 `finally` 中释放 extractor，允许下一任务重新启动。

### 11.3 OCR 行级选择与资源生命周期

Vision 保留逐行 `OcrLine`，不在引擎层提前丢失 box/confidence。GUI 候选区形成 `SubtitleProfile`；pipeline 根据文字系统、Y 区间、字号高度和中心距离选行，对段内最多 4 个代表帧做相似文本聚类与 medoid 共识。低置信文本只有在跨帧支持充足时放行。Vision 调用包在 `autorelease_pool` 中，CGImage 数据用 CFData 管理，避免长流处理时 Objective-C 临时对象堆积。

## 12. 架构决策

完整决策记录见 [DECISIONS.md](DECISIONS.md)，要点：

- **ADR-0001**：分层架构 = 三能力模块 + 串联层 + 入口层
- **ADR-0002**：Apple Vision 经 PyObjC 桥接，不引入 Swift helper
- **ADR-0003**：Phase 1 范围确定为可运行 CLI MVP
- **ADR-0004**：任务粒度合并为 10 个粗任务，subtasks 字段承载细节
- **ADR-0005**：Phase 2 GUI 架构 = SwiftUI 壳 + Python UDS 子进程
- **ADR-0006**：Phase 2 抽帧双方案 = AVFoundation 主路径 + 系统 ffmpeg 兜底
- **ADR-0007**：Pipeline `run_frames()`、Swift 端 SRT、JSON 替代 MsgPack
- **ADR-0008**：Vision 候选框 + 用户多选替代手动画框
- **ADR-0009**：跳过 Phase 2 `.app` 打包与 notarization
- **ADR-0010**：GUI 默认打轴抽帧统一到 Python `FfmpegExtractor`
- **ADR-0011**：OCR 文字系统默认 `auto`，显式 CJK 仅做边界 cleanup
- **ADR-0012**：增量 pipeline、真实进度与取消共享同一任务生命周期
