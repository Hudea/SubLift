# Phase 4.2 — OCR 内部性能归因与决策基线

> 设计源头：[OCR 内部性能归因设计](../design/ocr-performance-attribution.md)。任务状态与
> 验证证据以 [phase4.2.json](../phases/phase4.2.json) 为准。

## 1. 主题、目标与为什么现在做

**主题：**把现有粗粒度 `ocr` wall 扩展为可复现、可对账的 Vision 内部计时树，并关联到每段
字幕的代表帧和早停决策。

**目标：**在不改变任何产品行为的前提下，得到一个可信 baseline，能确定下一项优化究竟应
落在 Vision request、PIL/CGImage 桥接、段内 OCR 调用数、行级选择/共识，还是根本不应继续
做 OCR 优化。

这不是“让 OCR 更快”的 feature。速度提升不作为本 feature 的完成条件；可解释、低扰动和
质量等价才是完成条件。

Phase 4.1 已给出立项依据：重叠 producer/consumer 在真实 Vision 下结果正确，却未稳定跨过
`end_to_end_wall ≤ serial × 95%` 的最低门。ROI 后 producer 很快填满有界队列，单消费者
OCR 是更值得首先量清的对象。

## 2. 范围

### 做

- 扩展 `PerformanceRecorder`、Pipeline 逐段 trace 与报告 schema；
- Vision 内部计时：输入准备/CGImage、request 设置、`performRequests`、observation 映射、
  residual/自动释放；
- 记录段内代表帧数、实际 OCR 调用数、输入尺寸、早停原因、行选择/共识成本与接受结果；
- 对 canonical Vision ROI 负载形成有数据、有质量快照的正式归因报告；
- 给出一个由数据驱动的下一 feature 候选，或明确结论“暂无值得做的 OCR 优化”。

### 不做

- 不改 OCR 语言、置信阈值、`ocr_consensus_frames`、行选择、共识、SSIM、打轴、ROI、抽帧或 IPC 调度；
- 不重开 Phase 4.1 producer/consumer，也不并行 Vision；
- 不添加 GUI 性能开关，不采集/提交视频、图像、OCR 文本或绝对路径；
- 不把 Zootopia 单素材性能读数宣称为跨片源泛化，也不把英文/混排/不同位置 GT 扩充混入本 feature。

## 3. feat-043 子任务与执行顺序

| 顺序 | 子任务 | 交付物 | 停止条件 |
|---:|---|---|---|
| 043a | 归因数据模型与对账 | 有界 `ocr_breakdown`、段级 trace schema、fake-clock 单测 | parent/child 双计或数据无界则先修模型，不接 Vision |
| 043b | Vision 可选内部计时 | 不改变 `OcrEngine` Protocol 的 observer 接入；空/错误也完整结束记录 | 任一 off/summary/trace 结果 hash 不一致则停止 |
| 043c | 段内决策 trace | 代表帧收集耗时、每次匿名调用、早停原因、accept/reject 与既有 line-select/consensus 对齐 | trace 泄露文本/图像/路径或调用数不守上限则停止 |
| 043d | benchmark 与报告 | canonical Vision summary/trace 基线、扰动对照、质量门、正式报告与唯一下一项建议 | 指标不可解释或缺真实 Vision 数据则不能收口 |

## 4. 验收目标（全部为硬门，除非明确标为报告结论）

### 4.1 行为与质量不变

| 门 | 要求 |
|---|---|
| Protocol | `OcrEngine.recognize(image)` 签名不变；Mock 与未来非 Vision 引擎不因归因而失效 |
| off 语义 | mode=off 不创建 recorder/observer，不写 trace，不改变产品默认路径 |
| 结果等价 | 同一合成帧流的 off、summary、trace entries 完全一致；canonical Vision measured runs 的 `detection_hash` 完全一致 |
| 固定 GT | 每次 canonical Vision measured run 均通过 F1 ≥95.2%、precision ≥98.8%、usable ≥85.1%、CER macro ≤6.6%、noise ≤2、empty ≤1 |
| 异常语义 | Vision 空结果、`performRequests` 失败、observer 不可用时，OCR 原有返回/异常语义不变，记录标为 `empty`/`error`/`opaque` |

### 4.2 归因正确、完整且不双计

| 门 | 要求 |
|---|---|
| 调用数 | Vision summary 中 `ocr_breakdown.call_count == stages.ocr.count == throughput.ocr_calls`；每段 trace 调用数总和与该 run 一致 |
| parent 对账 | 每次 Vision call 均有五个内部时间项；`components + residual` 与 parent `ocr` 总 wall 的差 ≤ `max(0.1 ms, parent×1%)`，并在 payload 给出 delta |
| 核心 coverage | 内部阶段不得加入 `stages` 或 `_COVERAGE_LEAF_STAGES`；新增字段前后同一 fake-clock 测试证明 coverage 不把 OCR 重算 |
| 段级完整性 | 每个 closed segment 都有 `representative_selection_ms`、`early_stop_reason`、实际调用数和接受结果；`ocr_call_details ≤ ocr_consensus_frames` |
| 有界性 | summary 的阶段与输入尺寸样本均有固定 cap；trace 单段最多保存实际调用数（≤配置 cap）；≥10 分钟样本不会因计时数据线性占用内存 |
| 隐私 | `.perf-segments` / agent JSON 不出现 OCR 文本、图片 bytes、OCR box、视频绝对路径；自动测试用敏感 sentinel 字符串扫描产物 |

### 4.3 基线与低扰动

协议固定为 canonical Zootopia 1080p、固定 ROI `[0,848,1920,87]`、5fps、Vision、cjk、同机 clean
commit、warmup=1 + measured=3、独立进程、median 为主统计，并归档 min/max、环境、hash 与质量。

| 门 | 要求 |
|---|---|
| summary 扰动 | `summary core_wall median / off core_wall median ≤ 1.05`；不通过时先减小测量成本 |
| trace 扰动 | `trace core_wall / off core_wall ≤ 1.10`（trace 仅开发者诊断，不可作为产品速度数字） |
| 真实 Vision 产物 | 至少一组 trace 和一组 summary 的 agent JSON/JSONL 产生，并包含 P50/P95、输入尺寸、早停分布和 parent 对账 |
| 正式报告 | 新增 `docs/reports/phase4.2-ocr-attribution-baseline.md`：明确环境、命令、质量、扰动、P50/P95、按总耗时排序、限制，以及一个下一 feature 建议或“不优化”结论 |

### 4.4 自动验证与收口

必须新增/更新单测覆盖：fake-clock 对账与无双计、bounded samples、trace schema/隐私、早停原因、
非 Vision opaque fallback、Vision helper 单元测试、off/summary/trace entries 等价、benchmark
aggregation/report schema。收口前实际运行：

```bash
uv run ruff check .
uv run mypy src tests
uv run pytest
./init.sh
cd apps/macos && swift test
```

真实 Vision benchmark 命令、报告位置、输出 commit/dirty 状态和每个硬门结果必须写入
`docs/phases/phase4.2.json`。任何一个行为、质量、对账、隐私或扰动硬门失败时，`feat-043`
保持 `in-progress`，不以“已经有一些计时”标记完成。

## 5. 完成后的决策

完成只代表“有可信证据”，不代表已经优化。报告必须按下列原则选下一步：

1. `vision_perform` ≥ OCR parent 的 70%：研究有效调用数/输入几何，先补多源 GT 后再动算法；
2. `input_prepare + request_setup` ≥20%：独立评估桥接与请求构造优化；
3. observation mapping、行选择、共识合计 ≥20%：只优化 Python 路径，且把混排风险纳入新 feature；
4. OCR 不是 core 最大项：停止 OCR 优化，转向相应主导 stage。

这四条是分流准则，不是自动改代码的授权。
