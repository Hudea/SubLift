# Phase 3 设计与执行计划

> 本文件是 Phase 3 的设计源头与范围边界。任务跟踪见 `docs/phases/phase3.json`，
> 项目级功能块见 `feature-list.json` 的 `phases[phase3]` 块。
>
> Phase 3 聚焦「优化」：建立可量化的 benchmark，改增量处理与前台进度体验，
> 优化打轴检测准确率。具体实现方法不在本阶段规划，留到各任务启动时再定。

## 1. 目标与范围

### 1.1 Phase 3 目标

让 SubLift 从「功能跑通」进入「基本可用」：

1. **可衡量**：建立端到端 benchmark，任何算法改动都能输出客观指标。
2. **可感知**：CLI/GUI 提取过程有真实进度反馈，用户能取消，首条字幕尽早出现。
3. **更准确**：打轴分段准确率提升，在 benchmark 数据集上达到验收门。
4. **更干净的 OCR 文本**：在已选字幕区域内进一步筛掉背景文字、伪文字和非目标字幕层。

### 1.2 在范围内

- **Benchmark 优化**
  - 定义核心指标：打轴精准度、识别准确率、端到端可用性、处理速度（× 实时）。
  - ground truth 加载与字幕条目对齐。
  - 一键运行 benchmark 的入口脚本。
  - 结构化诊断报告（agent JSON/GT CSV/detection CSV/summary）。
  - 记录当前基线，作为后续优化对比依据。

- **增量处理 + 前台进度显示优化**
  - CLI 提取时输出阶段进度/进度条。
  - GUI 提取时显示阶段 + 百分比。
  - 取消操作在提取过程中生效。
  - 缩短首条字幕反馈时间（具体目标值由 feat-029 实测后写入验收）。

- **打轴检测优化**
  - 仅优化 `pipeline/` 中打轴相关模块（`signature` / `changepoint` / `timeline`）。
  - 通过 benchmark 验证 `timing_f1` 提升，且 failure clusters 可解释。

- **OCR 字幕层筛选**
  - 区分 `region_box` 与 `subtitle_profile`：前者表示“看哪里”，后者表示“相信 ROI 里的哪一层文字”。
  - 保留 OCR per-line observation（text / bbox / confidence），避免在 OCR 引擎层过早合并所有文字。
  - 由 selector 根据 y 轨道、行高、最多行数、中心性、脚本提示、持久背景文字策略选择目标字幕行。
  - GUI 选中的候选框用于生成 profile；字幕宽度允许逐句变化，不把 x 范围作为硬约束。

### 1.3 不在范围内（后置）

以下功能明确不纳入 Phase 3，避免范围发散：

- OCR 区域裁剪优化（如调 `bottom_ratio`、自适应检测）仍不作为打轴任务处理；已选 ROI 内的字幕层筛选纳入 feat-033。
- ASS / VTT 导出完整实现。
- PaddleOCR 第二引擎接入与跨平台抽象。
- `.app` 打包与 Apple 公证（feat-025 保持跳过）。
- CLI 配置文件（`sublift.toml`）与复杂参数增强。
- 字幕翻译、软字幕提取、实时直播流处理。

## 2. 验收门

### 2.1 Benchmark 优化

- [ ] 一条命令即可对「视频 + ground truth SRT」输出完整指标。
- [ ] 报告至少包含 timing recall/precision/F1、边界误差、CER macro/micro、字符准确率、端到端可用召回、速度（× 实时）。
- [ ] 报告输出 agent 可读 JSON 与逐条 GT/detection CSV，可直接定位漏检、合并、过切分、OCR 空文本和高 CER。
- [ ] 记录当前基线（Zootopia clip 等现有素材）到 `debug/benchmark-reports/` 或 `docs/benchmarks/`。
- [ ] benchmark 脚本通过 `./init.sh` 验证（不引入 lint/type/test 回归）。

### 2.2 增量处理 + 前台进度显示优化

- [ ] CLI `sublift extract` 输出阶段进度（如抽帧 / 检测 / OCR / 导出）。
- [ ] GUI 提取时显示百分比和当前阶段，且取消按钮生效。
- [ ] 记录首条字幕反馈时间 benchmark，与 Phase 2 批量模式对比。
- [ ] Python 端 `uv run pytest` / `uv run ruff check .` / `uv run mypy src tests` 全绿；Swift 端 `swift test` 全绿。

### 2.3 打轴检测优化

- [ ] 在 benchmark 数据集上，`timing_f1` 提升到 ≥ 95%。
- [ ] precision 不下降（不出现新的误检）。
- [ ] 记录具体改善了哪些场景（如相似中文文本漏分段）。

### 2.4 OCR 字幕层筛选

- [ ] OCR 引擎或适配层能保留每行 observation 的文本、置信度和 bbox。
- [ ] GUI/Python IPC 支持可选 `subtitle_profile`；未传 profile 时保持现有行为。
- [ ] selector 能在同一 ROI 内过滤背景英文/水印/伪文字，只输出目标字幕层文本。
- [ ] benchmark 或 fixture 记录 CER/WER、重复条目变化和至少一个被改善的干扰场景。

## 3. 大功能块与任务拆分

| id | 任务 | 目标 | 依赖 | 验收 |
|---|---|---|---|---|
| **feat-027** | Benchmark 框架 | benchmark | — | 定义诊断指标、ground truth 加载、一键运行、输出诊断报告 |
| **feat-028** | Benchmark 基线录入 | benchmark | feat-027 | 对现有素材跑通 benchmark，记录基线数值 |
| **feat-029** | 增量处理架构 | incremental | feat-028 | Pipeline/IPC 支持边收帧边处理；首条反馈时间缩短 |
| **feat-030** | 前台进度与取消 | incremental | feat-029 | CLI/GUI 显示阶段进度；取消按钮生效 |
| **feat-031** | 打轴检测优化 | timeline | feat-028 | `timing_f1` ≥ 95%，`timing_precision` 不下降 |
| **feat-033** | OCR 字幕层筛选 | ocr-layer | feat-028 | 已选 ROI 内过滤背景文字/伪文字，只输出目标字幕层 |
| **feat-034** | 持久背景文字过滤 | ocr-layer | feat-033 | 跨段过滤 ticker / 水印等持久背景文字 |
| **feat-032** | Phase 3 文档收尾 | docs | feat-030, feat-031, feat-033, feat-034 | ARCHITECTURE/REQUIREMENTS/README/DECISIONS 更新 |

> 具体实现方法（如是否常驻 server、是否重构 Pipeline 为 push 模型、是否加 pixel-diff 信号等）
> 不在本计划阶段定死，由各任务启动时根据实测和约束选择。

## 4. 执行顺序

```
feat-027 (Benchmark 框架)
    │
    ├─ feat-028 (基线录入)
    │       ├─ feat-029 (增量处理架构)
    │       │       └─ feat-030 (前台进度与取消)
    │       │
    │       └─ feat-031 (打轴检测优化)
    │
    └─ feat-033 (OCR 字幕层筛选)
            │
            ├─ feat-034 (持久背景文字过滤)
            │
            └─ feat-032 (文档收尾)
```

**原则**：先建 benchmark，再用 benchmark 驱动增量和打轴优化，最后文档收尾。

## 5. 关键约束

- **允许修改 Phase 1 核心**：Phase 3 不再遵循 Phase 2 的「pipeline/ 零修改」约束，允许重构 `src/sublift/pipeline/` 和 `src/sublift/ipc/bridge.py` 以支持增量。
- **双端验证**：任何改动必须保持 Python 端 `./init.sh` 与 Swift 端 `swift test` 全绿。
- **不预先绑定实现**：本阶段只定目标和验收；具体技术方案在每个 feat 启动时评估。
- **进度显示要真实**：不允许假进度条，进度必须对应实际处理阶段。

## 6. 风险与权衡

| 风险 | 影响 | 缓解 |
|---|---|---|
| 增量处理重构面较大 | 可能引入回归 | 先用 benchmark 稳住基线，再小步重构 |
| 打轴优化与增量改动冲突 | 同时改 pipeline 结构 + 算法，bug 难定位 | 先打轴后增量，或分分支并行 |
| 字幕层 profile 过拟合单一视频 | 过滤掉合法双语/英文字幕，或仍保留背景字 | profile 以 y 轨道/行高/行数为主，脚本提示只作为软约束；用 benchmark/fixture 覆盖单语、双语、英文目标字幕 |
| ground truth 素材不足 | benchmark 说服力不够 | 至少复用现有 Zootopia clip，并记录获取方式 |
| 首条反馈时间受 Vision 冷启动影响 | 优化效果有限 | 实测区分「server 启动」vs「算法首条」时间 |

## 7. 阶段验收门（Phase 3 整体）

- [ ] feat-027~028 完成：benchmark 可运行，基线已记录。
- [ ] feat-029~030 完成：增量处理跑通，CLI/GUI 进度与取消可用。
- [ ] feat-031 完成：`timing_f1` ≥ 95%。
- [ ] feat-033 完成：已选 ROI 内的背景文字/伪文字不再并入目标字幕。
- [ ] feat-034 完成：持久背景文字（ticker / 水印）可跨段识别并过滤。
- [ ] feat-032 完成：文档更新，main 分支 `./init.sh` 8/8 通过。
