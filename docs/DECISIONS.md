# 架构决策记录

记录项目中已确认的技术决策，按时间倒序排列。  
**`progress.md` 只保留最近 3 条决策，旧条目归档至此。**

---

## ADR-0010 打轴抽帧统一到 Python FfmpegExtractor（2026-07-09）

- **背景**：GUI 用 AVF+JPEG 推帧时，同 region/Config 下 timing F1 ~84–86%，而 CLI/benchmark live（`FfmpegExtractor`）达 95.2%（5fps）/ 96.4%（8fps）。日志证明 region 与 pipeline 默认参数一致，差在选帧相位、时间戳网格与 JPEG 有损。
- **决策**：
  1. **打轴采样唯一实现** = Python `FfmpegExtractor`（与 CLI / `run_benchmark` 同源）。
  2. GUI 默认 **path mode**：`start_job.video_path` 传本地路径，后端自抽帧；Swift **不再**为打轴推 JPEG frame 流。
  3. 保留 **frame mode**（无 `video_path`）作兼容/调试。
  4. AVF 仅用于 **预览与选区代表帧**，与打轴解耦。
- **理由**：验收与产品必须同一像素/时间戳序列；双端各抽帧必然漂移。
- **影响**：`bridge.py` path mode；`SubtitleExtractor` 默认 path；`docs/design/macos-gui.md` 抽帧章节；后续默认 fps 仍建议 5（速度），8 作高精度档。

---

## ADR-0009 移除 Phase 2 .app 打包与 notarization 流程（2026-07-06）

- **背景**：feat-025 原计划把 SwiftUI 工程打包为可分发 `.app`，并完成 Developer ID 签名 + `notarytool` 公证，使 Gatekeeper 放行。该流程需要 Apple Developer Program 会员、Developer ID 证书、embedded Python 运行时以及 hardened runtime 适配。
- **决策**：**跳过 feat-025**，不做 `.app` 打包、embedded Python、签名与公证。Phase 2 当前范围止于「可在开发者环境通过 SwiftPM 构建并运行」的 macOS GUI，不产出面向终端用户的独立 `.app` 分发包。
- **理由**：用户明确决定移除该流程；当前阶段优先完成 GUI 功能与文档收尾，避免引入证书、公证、embedded Python 等分发侧阻塞。
- **影响**：
  - `docs/phases/phase2.json` 中 feat-025 状态改为 `blocked`，并注明跳过原因；feat-026 依赖从 feat-025 改为 feat-024。
  - `feature-list.json` 中 `phase2.distribution` 状态改为 `blocked`。
  - `README.md` 与相关文档不再包含 `.app` 安装 / 分发段，仅描述 SwiftPM 构建运行方式。
  - 未来如需分发，可重新开启 feat-025 或作为独立 release 工程处理。

---

## ADR-0008 feat-022 字幕区域：Vision 候选框 + 用户多选（2026-07-05）

- **背景**：feat-022 原计划为用户在预览上手动画矩形并重新跑流水线。用户反馈手动画框负担高、画错易导致识别失败；HURDLES 已记录 `bottom_ratio=0.3` 裁太宽问题，方案 3 为 Vision 自适应区域检测。
- **决策**：
  1. **取消手动画框**；改为代表帧上 **Vision 自动检测全部文字候选框**，预览以**彩色编号框**叠加展示。
  2. 用户**多选**哪些候选框属于字幕（可排除新闻标题等误检）；支持多行字幕对应多框。
  3. **区域推算**：**Y** 取选中框 min~max（加 padding）；**X MVP 固定全宽**（`x=0, width=video_width`）。
  4. 检测与预览叠加在 **Swift 端**用 `VNRecognizeTextRequest` 实现（feat-022a）；合并后的 `region_box` 接入 `start_job` + `bridge` `FixedRegionDetector`（feat-022b，已完成）。
- **理由**：兼顾自动化与用户可控；比纯启发式合并更抗误检；比手动画框更低门槛。X 全宽避免裁掉长字幕行。
- **影响**：`RegionPicker`(手画) 改为 `RegionOverlay`+`RegionCandidateList`；`docs/phases/phase2.json` feat-022 重写；`docs/plans/phase2.md` 区域相关段落已同步（§6.2/§6.3/§7/§8.3/§9）。

---

## ADR-0007 Phase 2 GUI 三项设计决策（2026-07-04）

Phase 2 启动前对三项影响 feat-015/016/024 的设计点拍板：

### ADR-0007a Pipeline 帧流接入：新增 `run_frames()`（feat-016）

- **背景**：Phase 1 `Pipeline.run(video_path: Path)` 内部调 `self._extractor.extract(video_path)`，假设帧来自视频文件。Phase 2 GUI 模式下帧由 Swift 端 AVFoundation/ffmpeg 抽取后经 IPC 以 JPEG 字节流送入 Python，无 `video_path`。plan §5.3 原写「小幅改造由 feat-016 评估」，未定方案。
- **决策**：在 `Pipeline` 上新增 `run_frames(frames: Iterator[Frame]) -> list[SubtitleEntry]` 方法，与 `run(video_path)` 并列。`run(video_path)` 内部重构为先用 extractor 生成 frames 迭代器再调 `run_frames`，避免逻辑重复。`bridge.py` 接收 IPC JPEG 帧后重建 `PIL.Image` 组装 `Frame`，调 `run_frames`。
- **理由**：这是真实的接口需求（GUI 帧源不是文件），「Python 核心零修改」是 Phase 2 起初的理想化约束，最小侵入的扩展方法优于 bridge 重写编排逻辑（C 方案）或假 path 包装（B 方案）。`run_frames` 与 `run` 共享内部步骤，不破坏 Phase 1 CLI 行为。
- **影响**：`src/sublift/pipeline/core.py` 新增 `run_frames` 并重构 `run`；feat-016 bridge 直接调 `run_frames`；Phase 1 测试需保持通过（`run` 行为不变）。更新 `docs/plans/phase2.md` §5.3 与 `docs/phases/phase2.json` feat-016 描述。

### ADR-0007b SRT 导出：Swift 端直接实现（feat-024）

- **背景**：feat-024 原写「经 IPC 调 Python SrtExporter 或 Swift 端直接调 sublift.export 模块，双方案内选一」。编辑后的字幕条目已存在于 Swift 内存，走 IPC 往返 Python 无收益。
- **决策**：SRT 格式化在 Swift 端实现。Swift 维护 `entries: [SubtitleEntry]` 模型，导出时本地格式化为 SRT 文本写文件。
- **理由**：SRT 格式极简（序号 + `HH:MM:SS,mmm --> HH:MM:SS,mmm` + 文本 + 空行），无理由走 IPC 往返；且避免引入「为导出再发 IPC 请求」的状态机分支。
- **影响**：`apps/macos/Sources/SubLiftMac/Core/SrtFormatter.swift` 新增；不调 Python 导出。更新 `docs/phases/phase2.json` feat-024 描述。

### ADR-0007c/d MsgPack 评估与撤销（独立决策，2026-07-05）

- **背景**：feat-015 原计划在 IPC 层引入 MsgPack 替换 JSON（plan §4.2）。评估两个 Swift MsgPack 库后，发现都有嵌套解码 bug（详见 HURDLES）。
- **决策**：**不引入 MsgPack，继续用 JSON**（ADR-0007c 撤销引入，ADR-0007d 确认 JSON 为最终方案）。
- **影响**：此决策**独立于 feat-015 任务范围**。feat-015 仍需定义 7 类消息的 JSON schema + 双向单测，只是序列化层用 JSON 而非 MsgPack。feat-015 因此从「MsgPack 序列化」重定义为「IPC 协议 7 类消息 schema（JSON 序列化）」。

---

## ADR-0006 Phase 2 抽帧双方案：AVFoundation 主路径 + 系统 ffmpeg 兜底（2026-07-03）

- **背景**：Phase 2 GUI 需要从视频抽帧后通过 IPC 送 Python Pipeline。AVFoundation 是 macOS 原生方案，可利用 VideoToolbox 硬解且无需额外依赖；但 feat-012 spike 证明 AVFoundation 无法打开 mkv 容器（报错 `-11828` / `-12847`），而 SubLift 需要支持 mkv 输入。
- **决策**：
  1. **主路径**：mp4 / mov / H.264 / HEVC 走 `AVAssetReader` + `AVAssetReaderTrackOutput`，`CVPixelBuffer` → `CGImage` → JPEG q=0.85。
  2. **兜底路径**：`.mkv` 及 AVFoundation 拒绝的容器走系统 `ffmpeg`，命令 `ffmpeg -i input -vf fps=5 -f image2pipe -vcodec mjpeg -`，stdout 为 MJPEG 流，按 SOI/EOI marker 切帧。
  3. **运行时检测**：启动时 `which ffmpeg` 检测，缺失弹窗引导 `brew install ffmpeg`，UI 禁用 mkv 拖入。
- **理由**：AVFoundation 覆盖 macOS 最常见格式且零额外依赖；ffmpeg 是 mkv 的通用解。按扩展名路由简单可靠，避免 AVFoundation 失败后再回退的 ~1s 延迟。
- **影响**：
  - `apps/macos/Sources/SubLiftMac/Core/FrameSampler.swift` 负责 AVFoundation 路径。
  - `apps/macos/Sources/SubLiftMac/Core/FfmpegFallback.swift` 负责 ffmpeg 检测、MJPEG 切帧、mkv 抽帧与元数据探测。
  - `apps/macos/Sources/SubLiftMac/Core/VideoMetadata.swift` 对 mp4/mov 用 AVURLAsset，对 mkv 用 ffprobe。
  - `docs/plans/phase2.md` §5 更新为双方案描述。

---

## ADR-0005 Phase 2 GUI 架构：SwiftUI 壳 + Python UDS 子进程（2026-07-03）

- **背景**：Phase 1 已交付可运行的 CLI Pipeline（Python）。Phase 2 需要 macOS GUI，目标是复用 Phase 1 算法核心，避免重写。
- **决策**：
  1. **双工程布局**：`apps/macos/` 新建 SwiftUI 工程，`src/sublift/` Python 工程保持不变。
  2. **进程边界**：SwiftUI 主进程作为 UI 壳，通过 **Unix Domain Socket (UDS)** 与本机启动的 Python 子进程通信。
  3. **Phase 1 核心零修改**：`pipeline/` / `extractor/` / `detector/` / `ocr/` / `export/` / `models.py` / `config.py` 不改动；新增 `src/sublift/ipc/` 模块把 Pipeline 包装为 IPC handler。
  4. **序列化**：JSON（4 字节大端长度前缀分帧），见 ADR-0007c/d。
- **理由**：
  - 复用已验证的 Python 算法核心，降低 GUI 阶段风险。
  - UDS 是本地进程间通信的轻量方案，不依赖网络，Swift 与 Python 都原生支持。
  - 明确的分层边界为 Phase 3 跨平台留路：核心算法与 UI 壳解耦，未来可用 Tauri/Electron 替换 SwiftUI 而不动 Python 核心。
- **影响**：
  - `apps/macos/Sources/SubLiftMac/Core/PipelineClient.swift` 负责启动 Python 子进程与 UDS 通信。
  - `src/sublift/ipc/server.py` / `protocol.py` / `bridge.py` 负责 Python 端 IPC 服务。
  - `Pipeline.run_frames()` 新增（ADR-0007a），让 bridge 可以注入 Swift 送来的 JPEG 帧流。
  - `docs/ARCHITECTURE.md` 增加 Phase 2 章节描述双工程 + IPC。

---

## ADR-0004 任务粒度调整：22 细任务合并为 10 粗任务（2026-07-03）

- **背景**：初始 Phase 1 规划拆出 22 个细粒度任务（feat-001~022），实践中发现粒度过细，导航与跟踪成本高。
- **决策**：合并为 10 个粗任务（feat-001~010），每个粗任务 = 一个可独立验收的功能块；任务内部细节通过 phaseN.schema.json 新增的 `subtasks` 字段承载（name + description），不拆成独立任务。CLI extract 实现随 feat-009/010 接入；端到端真实视频验收作为 Phase 1 级验收门，不单列任务。feat-004（文档）移到末尾依赖 feat-010，确保实现稳定后再写文档。
- **理由**：粒度以「能否独立验收」为准；粗任务承载主验收，subtasks 承载实现细节，兼顾可跟踪性与导航效率。
- **影响**：`docs/phases/phaseN.schema.json` 新增 `subtasks` 字段；`docs/phases/phase1.json` 重写为 10 任务 + 17 subtasks；`feature-list.json` covers 对齐；`docs/plans/phase1.md` 任务表与执行顺序图重写。ADR-0001/0003 中对旧 task ID 的引用以本决策为准。

## ADR-0001 Phase 1 模块布局与分层（2026-07-03）

- **背景**：Phase 1 需建立项目底座，定义字幕提取的三能力模块与串联层。
- **决策**：采用「三能力模块 + 串联层 + 入口层」分层。
  - 能力模块：`detector`（字幕区域检测）、`extractor`（帧采样）、`ocr`（OCR，可插拔多引擎）。每个模块定义 Protocol + 默认实现，可热插拔。
  - 串联层：`pipeline`（端到端编排，含 timeline 变化点状态机与 dedupe 去重合并）、`export`（字幕导出，Phase 1 实现 SRT，ASS/VTT 留接口占位）。
  - 入口层：`cli`（argparse，零依赖起步，`sublift extract` 子命令）。
- **理由**：用户明确三模块划分（detector/extractor/ocr）；extractor 定位为「帧采样器」而非端到端编排器；串联逻辑由 pipeline 承担。模块低耦合高内聚，平台特定 API 只能出现在 `ocr/vision.py`，核心层只依赖 Protocol，为 Phase 3 跨平台留路。
- **影响**：`docs/plans/phase1.md` §2 模块布局、§5 抽象接口；`feature-list.json` 7 大功能块；`docs/phases/phase1.json` 任务跟踪（任务粒度后经 ADR-0004 调整）。

## ADR-0002 Apple Vision 经 PyObjC 桥接接入（2026-07-03）

- **背景**：Phase 1 需选定 OCR 默认引擎的接入路径。
- **决策**：Python 通过 `pyobjc-framework-Vision` 直接调用 `VNRecognizeTextRequest`，不引入 Swift helper 子进程。
- **理由**：Phase 1 是 CLI 核心定位，纯 Python 路径最快；PyObjC 桥接避免额外的构建/分发复杂度；Swift 留给 Phase 2 GUI。
- **影响**：`pyobjc-framework-Vision` 与 `pyobjc-framework-Quartz` 作为 macOS 可选依赖；`ocr/vision.py` 是平台特定 API 唯一容身处；导入失败需优雅降级提示。

## ADR-0003 Phase 1 范围确定为可运行 MVP（2026-07-03）

- **背景**：Phase 1「底座」的深度需明确，决定验收标准与工作量。
- **决策**：Phase 1 交付可运行 MVP——真实 1080p 视频经 `uv run sublift extract <video> -o out.srt` 产出可加载 SRT；而非仅抽象接口。
- **理由**：用户选择「可运行 MVP」选项。端到端可运行才能验证架构有效性，避免抽象底座脱离实际。
- **影响**：`docs/phases/phase1.json` 验收标准含真实视频产出 SRT 与三工具全绿；端到端验收作为 Phase 1 级验收门（任务粒度见 ADR-0004）。ASS/VTT、PaddleOCR 第二引擎、配置文件、进度展示等显式排除。


