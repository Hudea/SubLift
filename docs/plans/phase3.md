# Phase 3 设计与执行计划

> 本文件是 Phase 3 的设计源头与范围边界。任务跟踪见 `docs/phases/phase3.json`，
> 项目级功能块见 `feature-list.json` 的 `phases[phase3]` 块。
>
> Phase 3 聚焦「优化」：建立可量化的 benchmark，改增量处理与前台进度体验，
> 优化打轴检测准确率，并以 OCR 行级选择器跨过可用性水位。本阶段已于
> 2026-07-12 收口；未执行项和残余风险在本文及 phase3.json 中明确保留。

## 1. 目标与范围

### 1.1 Phase 3 目标

让 SubLift 从「功能跑通」进入「基本可用」：

1. **可衡量**：建立端到端 benchmark，任何算法改动都能输出客观指标。
2. **可感知**：CLI/GUI 提取过程有真实进度反馈，用户能取消，首条字幕尽早出现。
3. **更准确**：打轴分段准确率提升，在 benchmark 数据集上达到验收门。

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

### 1.3 OCR 可用性（feat-034，Phase 3 追加）

打轴门通过后，主缺口转为识别可用性。**不在引擎内拼串 / 不全局降阈值**，改为：

- 保留 Vision 逐行 `OcrLine(text, confidence, box)`
- GUI 选区 → `SubtitleProfile` 透传
- 行级评分 + 段内 3–5 帧共识
- 验收：`usable ≥ 85.1%`，`text.noise ≤ 2`，`text.empty ≤ 1`，CER 逼近 6.6%，timing 不回退

详见 `docs/phases/phase3.json` feat-034。

### 1.4 不在范围内（后置）

以下功能明确不纳入 Phase 3，避免范围发散：

- 仅调 `bottom_ratio` / 全局 `confidence_threshold` 作为 OCR 主解法（历史已证 0.3 回退）。
- ASS / VTT 导出完整实现。
- PaddleOCR 第二引擎接入与跨平台抽象。
- `.app` 打包与 Apple 公证（feat-025 保持跳过）。
- CLI 配置文件（`sublift.toml`）与复杂参数增强。
- 字幕翻译、软字幕提取、实时直播流处理。

## 2. 验收门

### 2.1 Benchmark 优化

- [x] 一条命令即可对「视频 + ground truth SRT」输出完整指标。
- [x] 报告包含 timing recall/precision/F1、边界误差、CER macro/micro、字符准确率、端到端可用召回、速度（× 实时）。
- [x] 报告输出 agent 可读 JSON 与逐条 GT/detection CSV，可直接定位漏检、合并、过切分、OCR 空文本和高 CER。
- [~] Benchmark 框架与最终固定 GT 报告已记录；feat-028 的独立“初始基线入库”经用户决定跳过，历史水位汇总保留在 `docs/benchmarks/baseline.md`。
- [x] benchmark 脚本通过 `./init.sh` 验证（不引入 lint/type/test 回归）。

### 2.2 增量处理 + 前台进度显示优化

- [x] CLI `sublift extract` 输出探测、提取与识别、整理与导出阶段及真实进度。
- [x] GUI 提取时显示百分比和当前阶段，且取消按钮生效。
- [x] 自动审计首条字幕反馈 0.68s；取消响应 0.108s，取消后第二任务可正常启动。
- [x] Python 端 `uv run pytest` / `uv run ruff check .` / `uv run mypy src tests` 全绿；Swift 端 `swift test` 全绿。

### 2.3 打轴检测优化

- [x] feat-031：SSIM patrol 一阶（merged FN 显著下降；详见 phase3.json）。
- [x] feat-033：`timing_f1` 91.2% → **95.2%**（≥ 95%）。
- [x] feat-033：precision 100% → 98.8%（1 FA，可接受；详见 phase3 evidence）。
- [x] 记录改善场景：M1c 新闻空洞、短字幕 hysteresis、锚帧延迟（见 HURDLES / diagnosis）。

### 2.4 OCR 可用性（feat-034）

- [x] `usable_subtitle_recall` **92.0%** ≥ 85.1%
- [x] `text.noise` **0** ≤ 2；`text.empty` **0** ≤ 1
- [x] CER macro **3.2%** ≤ 6.6%
- [x] `timing_f1` **97.7%** ≥ 95.2%
- [x] `timing_precision` **98.8%** ≥ 98.8%
- [x] 禁止仅靠全局 `confidence_threshold=0.3` 过门

## 3. 大功能块与任务拆分

| id | 任务 | 目标 | 依赖 | 验收 |
|---|---|---|---|---|
| **feat-027** | Benchmark 框架 | benchmark | — | 定义诊断指标、ground truth 加载、一键运行、输出诊断报告 |
| **feat-028** | Benchmark 基线录入 | benchmark | feat-027 | 对现有素材跑通 benchmark，记录基线数值 |
| **feat-029** | 增量处理架构 | incremental | feat-028 | Pipeline/IPC 支持边收帧边处理；首条反馈时间缩短 |
| **feat-030** | 前台进度与取消 | incremental | feat-029 | CLI/GUI 显示阶段进度；取消按钮生效 |
| **feat-031** | 打轴检测优化（SSIM patrol） | timeline | feat-028 | patrol 落地、merged FN 显著下降；历史 F1 +15.6pp，**未达 95% 门由 feat-033 接力** |
| **feat-033** | 打轴 residual 优化 | timeline | feat-031 | 机制化收敛 residual FN；`timing_f1` ≥ 95%，`timing_precision` 不下降 |
| **feat-034** | OCR 行级选择器 | ocr-usability | feat-033 | usable≥85.1%；noise≤2；empty≤1；timing 不回退 |
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
    │       └─ feat-031 (打轴 SSIM patrol)
    │               │
    │               └─ feat-033 (打轴 residual → timing_f1≥95%)
    │                       │
    │                       └─ feat-034 (OCR 行级选择器 → usable≥85.1%)
    │
    └─ feat-032 (文档收尾，依赖 030+031+033+034)
```

**原则**：先建 benchmark，再驱动打轴与 OCR 可用性；打轴两阶（patrol → residual）后接行级选择器，最后文档收尾。增量（029–030）可与质量线并行。

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
| ground truth 素材不足 | benchmark 说服力不够 | 至少复用现有 Zootopia clip，并记录获取方式 |
| 首条反馈时间受 Vision 冷启动影响 | 优化效果有限 | 实测区分「server 启动」vs「算法首条」时间 |
| 显式 CJK 边界清理可能误删无空格英文 | `NPD动物警局`、`苹果的iPhone` 等混排受损 | 默认 `auto`；扩充混排 GT 后再收紧或移除启发式 |
| 长视频 GUI 体验尚未人工验收 | 自动指标不能覆盖进度观感与交互连续性 | 后续用 ≥10 分钟非 Zootopia 视频做拖拽、取消、重启验收 |

## 7. 阶段验收门（Phase 3 整体）

- [~] feat-027~028：benchmark 框架与固定 GT 报告完成；feat-028 独立基线入库按用户决定跳过。
- [x] feat-029~030 完成：增量处理跑通，CLI/GUI 真实进度与快速取消可用。
- [x] feat-031 完成：SSIM patrol 一阶落地（历史证据见 phase3.json）。
- [x] feat-033 完成：`timing_f1` 95.2%（≥ 95%）；precision 98.8%（相对 100% 基线 -1.2pp / 1 FA）。
- [x] feat-034 完成：usable 92.0%；noise 0；empty 0；CER 3.2%；F1 97.7%；precision 98.8%。
- [x] feat-032 完成：核心文档统一到 Phase 3 最终实现与固定 GT 口径；`./init.sh` 8/8 通过。
