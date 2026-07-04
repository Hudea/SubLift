# 架构决策记录

记录项目中已确认的技术决策，按时间倒序排列。  
**`progress.md` 只保留最近 3 条决策，旧条目归档至此。**

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

### ADR-0007c Swift 端 MsgPack：手写编解码，不引第三方库（feat-015）

- **背景**：plan §4.2 原指定用 `swift-msgpack` 库，但未验证维护状态、Codable 支持、SwiftPM 可用性，且会成为 feat-025 公证的第三方依赖风险点。
- **决策**：Swift 端手写 MsgPack 编解码。5 类消息（start_job/frame/cancel_job/progress/entries/log/done）字段固定，用 `Data` + 固定字节序手写 pack/unpack，双向单测覆盖。
- **理由**：零第三方依赖，公证风险最小，符合 AGENTS.md「低耦合」原则。MsgPack 协议本身简单（nil/bool/int/str/bin/array/map 各 1 字节类型 tag + 长度 + 负载），手写成本低于评估一个库的成本。
- **影响**：`apps/macos/Sources/SubLiftMac/Core/MsgPack.swift` 新增；`apps/macos/Tests/` 增编解码单测。更新 `docs/plans/phase2.md` §4.2 与 `docs/phases/phase2.json` feat-015 描述。

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


