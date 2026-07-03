# 架构决策记录

记录项目中已确认的技术决策，按时间倒序排列。  
**`progress.md` 只保留最近 3 条决策，旧条目归档至此。**

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


