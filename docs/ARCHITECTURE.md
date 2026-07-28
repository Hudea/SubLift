# SubLift 架构设计

## 1. 设计原则

- **模块化、可插拔、低耦合高内聚**：各能力模块定义 Protocol，默认实现可替换
- **平台 API 隔离**：平台特定 API（Apple Vision）只能出现在 `ocr/vision.py`，核心层只依赖 Protocol
- **串联层不实现单一能力**：pipeline / export 组合能力模块，处理流程逻辑

## 1.1 Phase 6 — Native C++ Core（默认已切换，hardening 进行中）

> **当前产品默认路径：vision / mock 使用 C++ Worker；paddle 使用 Python Worker。**
> SwiftUI + UDS 边界保留；Python Runtime 继续承担 paddle、冻结 Oracle 与 `SUBLIFT_RUNTIME=python` 回滚。产品 C++ Worker 需要 OpenCV 签名流水线；`SUBLIFT_ENABLE_OPENCV=OFF` 仅产出不宣告 engine/capability、明确拒绝作业的 sanitizer 诊断 Worker，不能作为 runtime 回退。相关计划见 `docs/cpp/phase6.6-hardening.md` 与 `docs/cpp/phase6.6-sanitizer-isolation.md`。

| | 说明 |
|---|---|
| 计划与契约 | **[`docs/cpp/`](cpp/README.md)**（总览、architecture、parity、worker-ipc、引擎矩阵/cutover） |
| 任务跟踪 | [`docs/phases/phase6.json`](phases/phase6.json) |
| 行为 Oracle | **冻结** `oracle_commit` + golden（见 `docs/cpp/parity-contract.md`），不是未钉扎的 main 尖端 |
| 引擎 cutover | vision/mock → C++ Worker；paddle → 仍 Python（见 `docs/cpp/engine-matrix-and-cutover.md`） |
| 实现树 | `cpp/`（自 feat-06002 起） |

Python 侧 `docs/design/*` 在 cutover 前仍是算法语义叙述源；与 C++ 冲突时以冻结 golden 为准。

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
    paddle.py              # PaddleOcrEngine（rapidocr / PP-OCRv6，可选）
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
- [ocr 设计](design/ocr.md) — Protocol、Vision / Paddle 实现、Mock、跨平台演进
- [extractor 设计](design/extractor.md) — Protocol、ffmpeg 实现、流式采样
- [benchmark 设计](design/benchmark.md) — 质量诊断、manifest 编排、性能模式；用法见 [benchmark/README.md](../benchmark/README.md)
- [ROI 数据通路设计（Phase 4 计划）](design/roi-data-path.md) — 固定区域 crop-before-Python、坐标契约与 A/B 验收边界
- [Phase 4 ROI 性能优化报告](reports/phase4-roi-performance.md) — 正式 clean-commit A/B 结果、性能结论与适用边界
- [版本化 benchmark 基线](../benchmark/reports/README.md) — 固定 GT 质量锚与当前 ROI 性能归因快照
- [Path-mode 有界重叠设计（Phase 4.1 已归档）](design/path-mode-overlap.md) — producer / consumer 所有权、取消、进度与并发性能口径；真实 Vision 未达吞吐门，未采纳
- [OCR 内部性能归因（Phase 4.2）](reports/phase4.2-ocr-attribution-baseline.md) — 已完成的 Vision 调用内部树、段级 trace 与下一步优化分流

## 5. 核心数据模型

`src/sublift/models.py` — 全部 `@dataclass(frozen=True)`，跨模块共享，避免循环依赖。

| 模型 | 字段 | 用途 |
|---|---|---|
| `BoundingBox` | `x, y, width, height: int` | 矩形区域；坐标空间由接口声明：外部选区 / manifest 为 source-frame，图像与 OCR box 为各自输入图局部坐标 |
| `Region` | `box: BoundingBox` | 字幕区域；坐标必须与其消费的 Frame.image 坐标空间一致 |
| `Frame` | `timestamp_ms: int, image: PIL.Image` | 视频帧，附带时间戳；image 的坐标原点为当前输入图左上 |
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
| opencv-python | 自适应二值化、形态学、SSIM；与 rapidocr 共用同一 `cv2` 发行包 | 必需 |
| pyobjc-framework-Vision | Apple Vision OCR | macOS 可选 |
| pyobjc-framework-Quartz | CGImage/CGDataProvider | macOS 可选 |
| rapidocr + onnxruntime | PaddleOCR（PP-OCRv6） | `paddle` 可选依赖，跨平台候选 |
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

固定 GT（Zootopia clip、1080p、5fps、统一 diagnostic 口径）的 Phase 3 最终结果：timing recall **96.6%**、precision **98.8%**、F1 **97.7%**；CER macro **3.2%**、micro **2.4%**、字符准确率 **97.6%**；usable subtitle recall **92.0%**；noise/empty 均为 **0**；处理速度 **21.0×** 实时。版本化报告见 [质量基线](../benchmark/reports/quality-baseline.md)；本地原始产物位于 `debug/benchmark-reports/feat034_p1_fix2/`。

上述结果是固定回归锚点，不是跨片源泛化承诺。当前限制：

- **GT 多样性不足**：主要依赖 Zootopia 中文字幕片段；Phase 4 的 ≥10 分钟非 Zootopia 视频只完成了 GUI 体验验收，不是可量化质量 GT。英文、中英混排、不同字幕位置和不同片源仍未形成固定质量集。
- **混排边界风险**：显式 CJK 画像的边界 cleanup 仍可能误删 `NPD动物警局`、`苹果的iPhone` 一类无空格英文，默认 `auto` 可规避部分风险。
- **打轴 residual**：极短字幕和 merged cluster（如 #15）仍可能漏检或合并。
- **path mode 保持串行调度**：ROI 后的 ffmpeg 读取与 Pipeline/OCR 仍在同一 worker 中交替执行。Phase 4.1 重叠实验已验证机制正确但未稳定越过吞吐门，未采纳；Phase 4.2 已确认 Vision 请求执行主导，下一步先补多源 GT 后研究有效 OCR 调用。
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
- **平台 API 隔离**：Apple Vision 在 GUI 端仅用于字幕区域候选框检测（`VisionTextDetector.swift`）；Python OCR 由用户所选的 `ocr/vision.py` 或 `ocr/paddle.py` 执行。
- **无分发包**：Phase 2 不做独立 `.app` 打包与 Apple 公证（ADR-0009），GUI 通过 `swift run SubLiftMac` 在开发者环境运行。

## 11. phase3-opt-perf 架构

### 11.1 Benchmark 驱动

`scripts/run_benchmark_manifest.py` 以 manifest 固定视频、GT、区域与配置，输出 summary、agent JSON、GT CSV 和 detection CSV。质量诊断为一对一 temporal IoU + case 分类；可选 `performance` 模式（off/summary/trace）在同源 Pipeline 上聚合阶段耗时，并与同次质量门联合报告。设计见 [design/benchmark.md](design/benchmark.md)；命令、manifest 字段与回归锚点见 [benchmark/README.md](../benchmark/README.md)。

### 11.2 增量处理与取消

`Pipeline` 拆为 `feed(frame)`、`ocr_segment(segment)` 与 `finalize()`：抽帧时持续推进打轴，段闭合即 OCR 并通过 IPC `push_entry` 推送，结束时再做最终去重。GUI path mode 的 worker 在线程中读取 `FfmpegExtractor`，进度消息携带实际帧数、总帧估计和 `processing/finalizing` 阶段。取消同时设置持久化 cancellation event 并终止 ffmpeg 子进程；worker 在 `finally` 中释放 extractor，允许下一任务重新启动。

### 11.3 OCR 行级选择与资源生命周期

Vision 保留逐行 `OcrLine`，不在引擎层提前丢失 box/confidence。GUI 候选区形成 `SubtitleProfile`；pipeline 根据文字系统、Y 区间、字号高度和中心距离选行，对段内最多 4 个代表帧做相似文本聚类与 medoid 共识。低置信文本只有在跨帧支持充足时放行。Vision 调用包在 `autorelease_pool` 中，CGImage 数据用 CFData 管理，避免长流处理时 Objective-C 临时对象堆积。

## 12. Phase 4 ROI 数据通路

对 **已知固定**字幕区域，ffmpeg 在 stdout 前 exact crop，Pipeline 仅消费
frame-local 的 ROI 图像（`RoiPassthroughDetector` → Region `[0,0,w,h]` 零拷贝透传）。
默认 GUI path mode 有效固定 region 自动启用；无 region、legacy frame mode、BottomCrop
和未验证旋转映射维持全帧路径。benchmark 以内部 `frame_output_mode=full|roi` 做 A/B。

这不是 codec 级 ROI decode：编码帧通常仍需完整重建。目标是消除全帧 RGB 管道、PIL
materialize 与重复 crop 的无效成本，并用同提交 full / roi A/B 与固定 GT hash 验证结果
等价。架构契约见 [ROI 数据通路设计](design/roi-data-path.md)，任务与硬门见
[Phase 4 计划](plans/phase4-roi-data-path.md)。

## 13. Phase 4.1 ROI 后可重叠流式吞吐（已归档）

> `feat-042` 的实验实现已验证机制正确，但两轮真实 Vision A/B 未达到吞吐保留门，故未
> 合入 main。当前 path mode 仍保持第 11.2 节所述的单 worker 串行行为，不能据此把本节
> 当作已实现。

Phase 4.1 只对 GUI 默认 Python path mode 引入一个有界帧 FIFO：producer 独占
`FfmpegExtractor` / generator，consumer 独占 `Pipeline`、timeline 状态和 Vision OCR。
这会让抽帧与串行 Pipeline/OCR 重叠，但不并行 OCR、不改变帧序或增加 GUI 开关。

~~~text
PathJobSession(job_id, cancel_event, Queue(maxsize=8))
  ├─ producer: FfmpegExtractor.extract() ── Frame / EOF / error ─┐
  └─ consumer: Pipeline.feed() → ocr_segment() → finalize()      ├─→ coordinator → GUI
                                                                    ┘
~~~

每个 job 的 extractor、pipeline、threads、queue 和观测数据都必须 session-local，不能由
`BridgeHandler` 的跨 job 可变字段互相覆盖。进度按 consumer 已处理帧发送；EOF 被 consumer
消费后才 `finalize()`；取消须同时打断 ffmpeg read、满队列等待和 OCR 返回后的后续工作，且
producer、consumer 全部退出后才可以发送 `done`。性能报告将区分 end-to-end、producer 和
consumer lane；这些 lane 可以重叠，禁止相加为 coverage。

完整不变量、取消状态机与口径见 [Path-mode 有界重叠设计](design/path-mode-overlap.md)，实验
数据与归档决定见 [phase4.1.json](phases/phase4.1.json)。

## 14. Phase 4.2 OCR 内部性能归因（已完成）

Phase 4.2 已证明：在 canonical ROI 负载上，Vision `performRequests` 请求执行持续占 OCR
parent 的约 99%，而输入准备、request 设置与 observation 映射合计约 1%。它不改变调度、OCR
算法或 GUI；而是将 OCR 保持为一个**核心 coverage 叶子**，并在其下另存不参与 coverage 相加
的内部明细树。这样 `core_wall` 仍只计一次 OCR，内部各分项加 residual 后与 OCR parent 对账。

`summary` 只保留有界聚合（调用数、total/mean/max/P50/P95、输入尺寸直方图与早停原因
计数）；`trace` 才逐段保存最多 `ocr_consensus_frames` 条匿名调用记录，不写字幕文本、图像
或视频路径。Vision 为可选计时能力，`OcrEngine.recognize(image)` Protocol 不变，非 Vision
引擎以 opaque OCR 调用降级。summary 的本轮扰动未通过，故它不能作为产品速度基线；这不改变
归因结论。完整限制、证据和下一方向见
[Phase 4.2 正式报告](reports/phase4.2-ocr-attribution-baseline.md)。下一优化先补多源 GT，
再在质量门内实验代表帧排序与有效 OCR 调用数。

## 15. 架构决策

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
- **ADR-0013**：SSIM patrol 为内部默认机制，不暴露给 GUI 用户
- **ADR-0014**：固定字幕区域自动在 ffmpeg 输出前裁剪，Pipeline 只消费 frame-local 坐标（Phase 4 计划）
- **ADR-0015**：性能 coverage 以排他 `pipeline_overhead` 补齐批量编排时间，避免与 leaf stage 双计
- **ADR-0016**：未通过真实 A/B 吞吐保留门的 path-mode overlap 不进入 main；保留数据作为负向证据
- **ADR-0017**：OCR 内部明细是 `ocr` coverage leaf 的子树，不参与 core coverage 相加；归因完成后先补多源 GT，再实验代表帧排序与有效调用数
- **ADR-0018**：PaddleOCR 采用 rapidocr PP-OCRv6 + onnxruntime，模型缓存放在用户缓存目录；作为可选第二 OCR 引擎接入
