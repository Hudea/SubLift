# SubLift 架构设计

## 1. 设计原则

- **模块化、可插拔、低耦合高内聚**：各能力模块定义 Protocol，默认实现可替换
- **平台 API 隔离**：平台特定 API（Apple Vision）只能出现在 `ocr/vision.py`，核心层只依赖 Protocol
- **串联层不实现单一能力**：pipeline / export 组合能力模块，处理流程逻辑

## 1.1 Phase 6 — Native C++ Core（6.8 Paddle cutover 已完成）

> **当前实现路径：vision / mock 使用 C++ Worker；paddle 在 C++ adapter/模型可用时也默认
> 使用 C++ Worker，不可用时 fail-closed。只有显式 `runtime=python` 才进入 Python。**
> SwiftUI + UDS 边界保留；Python Runtime 继续承担冻结 Oracle、benchmark 与
> `SUBLIFT_RUNTIME=python` 一键回滚，至少保留一个小版本周期。
>
> Paddle Native 已对齐完整 Det DB/unclip、Quad crop、Cls、Rec 与 CTC；3 来源
> 614.272s 产品质量输出逐源 SHA exact。120s canonical C++ wall/RSS 分别为 Python 的
> `0.8956x/0.9152x`。开发 Release 构建把已验收 ORT 复制到 build tree 并使用相对 rpath；
> `.app` 随包模型、签名与公证按 ADR-0030 整体后置，不属于当前开发架构。
>
> 产品 C++ Worker 需要 OpenCV 签名流水线；`SUBLIFT_ENABLE_OPENCV=OFF` 仅产出不宣告
> engine/capability、明确拒绝作业的 sanitizer 诊断 Worker，不能作为 runtime 回退。相关计划见
> `docs/cpp/phase6.6-hardening.md`、`docs/cpp/phase6.6-sanitizer-isolation.md` 与
> `docs/cpp/phase6.8-paddle-hardening.md`。

| | 说明 |
|---|---|
| 计划与契约 | **[`docs/cpp/`](cpp/README.md)**（总览、architecture、parity、worker-ipc、引擎矩阵/cutover） |
| 任务跟踪 | [`docs/phases/phase6.json`](phases/phase6.json) |
| 行为 Oracle | **冻结** `oracle_commit` + golden（见 `docs/cpp/parity-contract.md`），不是未钉扎的 main 尖端 |
| 引擎 cutover | vision/mock/paddle 全引擎 → C++ Native CLI 经过 Worker UDS IPC 编排；Python CLI 保留为 Oracle 比对与回滚工具（见 `docs/cpp/engine-matrix-and-cutover.md`） |
| Native 开发架构 | 6.9 CMake/目录边界、Ports/Adapters、Worker、ResourceLocator 与薄 CLI 已收口；分发后置（见 `docs/cpp/phase6.9-native-product-architecture.md`） |
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
- [版本化 benchmark 基线](../benchmark/baselines/README.md) — 固定 GT 质量锚与当前 ROI 性能归因快照
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
| rapidocr + onnxruntime | Python Paddle Oracle / 回滚 | `paddle` 可选依赖 |
| C++ ONNX Runtime + OpenCV | Paddle Native Det/Cls/Rec 产品适配器 | `sublift_paddle` 私有依赖；不进入 `sublift_core` |
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

固定 GT（Zootopia clip、1080p、5fps、统一 diagnostic 口径）的 Phase 3 最终结果：timing recall **96.6%**、precision **98.8%**、F1 **97.7%**；CER macro **3.2%**、micro **2.4%**、字符准确率 **97.6%**；usable subtitle recall **92.0%**；noise/empty 均为 **0**；处理速度 **21.0×** 实时。版本化报告见 [质量基线](../benchmark/baselines/quality-baseline.md)；本地原始产物位于 `debug/benchmark/archive/legacy-runs/feat034_p1_fix2/`。

上述结果是固定回归锚点，不是跨片源泛化承诺。当前限制：

- **GT 多样性不足**：主要依赖 Zootopia 中文字幕片段；Phase 4 的 ≥10 分钟非 Zootopia 视频只完成了 GUI 体验验收，不是可量化质量 GT。英文、中英混排、不同字幕位置和不同片源仍未形成固定质量集。
- **混排边界风险**：显式 CJK 画像的边界 cleanup 仍可能误删 `NPD动物警局`、`苹果的iPhone` 一类无空格英文，默认 `auto` 可规避部分风险。
- **打轴 residual**：极短字幕和 merged cluster（如 #15）仍可能漏检或合并。
- **path mode 保持串行调度**：ROI 后的 ffmpeg 读取与 Pipeline/OCR 仍在同一 worker 中交替执行。Phase 4.1 重叠实验已验证机制正确但未稳定越过吞吐门，未采纳；Phase 4.2 已确认 Vision 请求执行主导，下一步先补多源 GT 后研究有效 OCR 调用。
- **ASS/VTT 仅占位**：当前产品导出 SRT。

## 10. Phase 2 macOS GUI 架构

Phase 2 最初采用 **SwiftUI 壳 + Python UDS 子进程**（ADR-0005）；Phase 6 cutover 后，
进程边界与 JSON 协议保持不变，默认后端已替换为 C++ `sublift_worker`。Python server
仅在显式 `runtime=python` 时作为冻结 Oracle 或开发回滚。

### 10.1 双工程布局

```
SubLift/
  cpp/                    # 默认产品 Core、Worker 与原生 adapters
  src/sublift/            # Python Oracle、benchmark 与兼容 IPC runtime
  apps/macos/             # SwiftUI 壳工程
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
| Swift 端 | `PipelineClient` 解析 runtime、启动对应 Worker，并通过 BSD socket 收发 |
| 默认后端 | `sublift_worker --socket <path> --engine <engine>`；C++ capability 缺失时 fail-closed |
| 显式回滚 | `python -m sublift.ipc.server --socket <path> --engine <engine>`；仅显式 Python runtime |
| 消息类型 | `start_job` / `frame` / `finalize` / `cancel_job` / `progress` / `push_entry` / `entries` / `log` / `done` |

### 10.3 Swift 端分层

| 层 | 文件示例 | 职责 |
|---|---|---|
| App | `SubLiftMacApp.swift`、`WorkspaceCommands.swift` | `@main` 入口、Workspace 生命周期、系统菜单与快捷键 |
| Core | `WorkspaceModel.swift`、`WorkspaceState.swift`、`PipelineClient.swift`、`SubtitleExtractor.swift`、`SubtitleEditor.swift`、`SrtFormatter.swift` | Session/命令真源、IPC、提取协调、最终编辑模型与导出 |
| UI | `WorkspaceRootView.swift`、`TranscriptPanel.swift`、`RegionOverlay.swift`、`QuickExtractionSettingsBar.swift`、`Settings/SettingsView.swift` | Native Shell、只读/可编辑 Transcript、区域、快速设置和分层偏好 |

### 10.4 GUI 数据流

```
用户 Open 或拖入视频
  ↓
VideoImportPolicy 校验 → WorkspaceModel 创建/替换 Session
  ↓
AVFoundation ──→ 预览与 Vision 候选框（不参与默认打轴采样）
  ↓
requestExtraction 冻结 active 配置 → PipelineClient.start_job(video_path, region_box, SubtitleProfile)
  ↓ UDS + JSON
默认 C++ Worker（或显式 Python Worker）path mode
  ↓
Worker-owned FfmpegExtractor ──逐帧推进──→ Pipeline.feed(frame)
  ↓
[段闭合时触发] push_entry 增量推送 → 只读 Live Transcript
  ↓
[全片结束] entries 最终批量合并推送 → SubtitleEditor.load(entries) + final 配置快照
  ↓
Review 中 TranscriptPanel 编辑 / Timeline 定位 / SrtFormatter.format() → NSSavePanel 写文件
```

默认 GUI 与 Native CLI 使用 C++ extractor；Python Oracle / benchmark 使用 Python extractor。
两条实现由冻结 extractor/parity 契约约束，而不是通过共享同一个 Python 实例保持一致。
Swift 推 JPEG 的 frame mode 仅保留为兼容与调试路径。

### 10.5 关键约束

- **运行时路由**：vision/mock/Paddle 默认 C++；Paddle capability 不可用时明确报错，不静默改引擎或回退 Python。
- **平台 API 隔离**：GUI 内 Apple Vision 只负责字幕区域候选框；实际 OCR 由选定 Worker 的 Vision/Paddle adapter 执行。
- **显式回滚**：`SUBLIFT_RUNTIME=python` 或等价显式参数才进入 Python，GUI 显示最终 runtime 身份。
- **无分发包**：Phase 2 不做独立 `.app` 打包与 Apple 公证（ADR-0009），GUI 通过 `swift run SubLiftMac` 在开发者环境运行。

## 11. phase3-opt-perf 架构

### 11.1 Benchmark 驱动

Benchmark 可执行代码位于正式包 `src/sublift/benchmark/`，配置、GT、冻结基线与
parity 资产分别位于仓库根 `benchmark/configs|datasets|baselines|parity`。固定本地媒体
保留在 `debug/` 根目录且不入库；imports、runs、perf 与 archive 写入 `debug/benchmark/`。
入口 `sublift-benchmark` 提供 `run / matrix / score / show`
以及 recorder 扰动、ROI 对照专项命令；历史脚本仅保留兼容转发。

`run/matrix` 使用同源 Python `Pipeline + FfmpegExtractor + OCR` 以保留阶段性能埋点；
`score` 对任意 C++/Python/GUI 已有 SRT 使用同一套一对一 temporal IoU、CER、usable、
case 分类与 JSON/CSV/Markdown 报告。v2 config 将单次 run 与参数矩阵分离，通用
`--set / --vary` 负责未来参数扩展并拒绝未知字段。设计见
[design/benchmark.md](design/benchmark.md)，命令与回归锚点见
[benchmark/README.md](../benchmark/README.md)。

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
数据与归档决定见 [phase41.json](phases/phase41.json)。

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

## 15. Phase 10 macOS Native Workbench UI（已实施）

Phase 10 保留 SwiftUI + UDS + 默认 C++ Worker 架构，只重组单窗口 Session 的状态所有权与
原生 macOS Surface。`WorkspaceModel/WorkspaceState` 作为组合层：负责 video、
metadata、player、region、extraction、transcript、selection 和 command availability；
现有 focused Core models 继续拥有 IPC、播放、坐标、编辑与导出逻辑，View 不复制业务实现。

主窗口采用 Video Workspace + Transcript Panel + 可选 Context Inspector，不增加永久左侧
Sidebar。Inspector 按 Video / Region / Extraction / Subtitle 切换；Settings 与视频区快速设置栏
编辑同一组跨 Session 偏好，Extraction Inspector 只读展示 active/final 配置。processing/finalizing
的 Live Transcript 必须只读，只有最终 `entries` 替换完成后才进入
可编辑 Review，从结构上消除最终结果覆盖处理中用户修改的风险。

本 Phase 不改变 runtime fail-closed、Worker path mode、ROI/坐标、算法或 UDS framing；也不
实现 Task Center/批量队列、自动引擎、Whisper、ASS/VTT、模型下载与独立分发。Task Center
已在后续 Phase 8 独立规划，不追溯改变 Phase 10 的范围。设计入口见
[docs/design_ui](design_ui/README.md)，实施计划见
[Phase 10 macOS Workbench UI](plans/architecture/phase10-macos-workbench-ui.md)，跟踪见
[phase10.json](phases/phase10.json)。

**实施状态（2026-08-12）**：Phase 10 已完成到 10416；产品实现止于 10415，完成 Session 状态模型、Welcome/导入、
Workspace Shell、Context Inspector、Region Editing、Transcript、Processing/Review、Timeline、
Settings、响应式/辅助功能、独立审计修复与快速提取设置栏；10416 已完成最终文档收口。
完整 `swift test`（201 XCTest + 140 Swift Testing）与标准门 10/10 全绿。V01–V09、960 紧凑、
Light/Dark、A01/A02 代码与自动测试证据见 `docs/phases/phase10.json` 和
`docs/design_ui/evidence/`；V10 系统设置切换、完整 VoiceOver 会话及部分真实点击受系统权限限制，
未伪装为已执行。

## 16. Phase 8 批量任务中心与文件夹导入（已实现）

Phase 8 在单视频 Workspace 之外增加独立 `BatchQueueModel` 组合根；它拥有多任务清单、输入
扫描、输出规划、串行调度、任务 Runner 与本地持久化，不把队列状态塞入 `WorkspaceModel`，
也不共享可变 `SubtitleExtractor`。

```text
SubLiftMacApp
├── WorkspaceModel          # 单视频预览、区域、提取、校对、手动导出
└── BatchQueueModel         # 多文件/文件夹、队列、自动 SRT、恢复
      ├── BatchInputScanner
      ├── BatchOutputPlanner + AtomicSrtWriter
      ├── BatchQueueScheduler (maxActive = 1)
      ├── BatchExtractionRunner → per-task PipelineClient
      └── BatchQueueRepository → versioned atomic JSON
```

首版固定单并发；暂停表示完成当前任务后停止调度，不 suspend Worker。任务在入队时复制
`ExtractionConfiguration`，waiting 可显式修改，preparing 后锁定；Settings 改动不追溯。
批量任务使用 `region_box=nil` 的 Worker 默认底部区域，逐文件 Region Editing 仍属于 Workspace。

输出默认是源视频同目录 sidecar SRT；公共输出目录保留文件夹相对结构。已有目标默认跳过，
也可稳定自动重命名或经集中确认后替换。最终 entries 经 `SrtFormatter` 写同卷临时文件并原子
落地；completed 后只保留字幕数量、输出和 runtime 摘要，避免长队列线性持有字幕。

队列清单使用 Application Support 下 versioned Codable JSON 原子保存；恢复时活动任务变为
interrupted、队列 paused，用户显式继续前不启动 Worker。Phase 8 不新增数据库、并行 OCR、
目录监听、Automatic/Whisper、新格式、Worker/IPC 或分发范围。产品合同见
[批量任务中心设计](design_ui/batch-task-center.md)，架构计划见
[Phase 8 计划](plans/architecture/phase8-batch-task-center.md)，跟踪见
[phase8.json](phases/phase8.json)。Phase 8 已全部完成（08001–08410，含综合验收）。

## 17. 架构决策

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
- **ADR-0024**：在去 Python/打包前插入 Phase 6.8，先完成 Paddle Native stage parity、多源质量门、性能与安全 cutover
- **ADR-0029**：Paddle Native 全门通过后正式默认 C++；ORT 以已验收 SHA 和相对 rpath 固化，Python 保留回滚
- **ADR-0035**：Phase 10 采用单视频 Native Workbench + Context Inspector；Live Transcript 在 final entries 前只读，未来能力不做假入口
- **ADR-0037**：Phase 8 采用独立 Task Center + 单并发队列；配置快照、安全 SRT 与显式 JSON 恢复不扩张 Workspace/Worker

## 18. Harness 协作与验证边界

项目的操作性进度索引为 `phases.json`，每个 `detail_file` 指向
`docs/phases/phase*.json` 的唯一 feature 记录。`.agent/` 当前只提供轻量规则、
session bootstrap/handoff 与提交辅助；复杂能力编排已经从活跃 Harness 移出，未来按
项目真实需要增量引入。历史 `feature-list.json` 和 `feat-*` 记录保留为文档/旧工具
兼容面，不再用于选择当前任务。

`./init.sh` 只检查 `AGENTS.md`、`progress.md`、`phases.json` 与其 `detail_file` JSON 链；
不安装依赖、不构建、不运行产品测试。完整产品日常门由 `scripts/verify-standard.sh` 承担。
此 Agent Harness 与 C++ parity/golden harness 是不同概念，后者仍是产品正确性测试契约。
Phase 7 现统一承载项目辅助架构：07001 记录初次 Harness 迁移，原 Phase 9 的
09001–09006 记录后续稳定化与仓库治理；吸收式迁移和 Phase 9 复用规则见 ADR-0036，
当前精简边界见 ADR-0033。
