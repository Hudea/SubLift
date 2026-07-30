# phase3-opt-perf 设计与执行计划

> 本文件是 Phase 3 的设计源头与范围边界。任务跟踪见 `docs/phases/phase3.json`，
> 项目级功能块见 `feature-list.json` 的 `phases[phase3]` 块。
>
> Phase 3 聚焦「opt/perf」：既有质量优化先建立可量化 benchmark、增量处理、
> 打轴与 OCR 可用性水位；当前继续建立开发者性能模式和固定负载性能 baseline。
> 2026-07-12 收口的质量成果保持为后续性能优化不可回退的锚点；实际性能机制优化
> 不在 feat-037 中预设，等 baseline 指出主瓶颈后再新增独立 feature。

## 1. 目标与范围

### 1.1 Phase 3 目标

Phase 3 前半段让 SubLift 从「功能跑通」进入「基本可用」，当前 opt/perf
延续段让性能优化也具备同样可量化、可回归的闭环：

1. **可衡量**：建立端到端 benchmark，任何算法改动都能输出客观指标。
2. **可感知**：CLI/GUI 提取过程有真实进度反馈，用户能取消，首条字幕尽早出现。
3. **更准确**：打轴分段准确率提升，在 benchmark 数据集上达到验收门。
4. **可剖析**：以低扰动开发者性能模式分解真实阶段成本，形成可复现性能 baseline，后续优化必须同时说明性能收益与质量变化。

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

### 1.5 收口后追加：GUI 处理倍速（feat-035）

用户在处理时除百分比外还需要判断吞吐量。复用 Python path mode 已推送的真实进度，
在 Swift 端计算 `已处理视频时长 / 实际处理耗时`，并显示为如 `4.0× 实时` 的处理倍速。
不新增 IPC 字段、不改变抽帧/OCR 行为，也不将采样 fps 或播放速度误报为处理性能。

### 1.6 收口后追加：GUI 左栏自适应布局（feat-036）

左侧预览不能无限占用纵向空间；它应保留播放控制、区域选择和提取操作的稳定可见空间。
预览高度同时受视频比例、可用高度和上限约束；提取栏在窄栏拆行并让状态文本单行截断。

### 1.7 opt/perf：开发者性能模式与性能基线（feat-037）

性能优化先测量，不直接猜测瓶颈。feat-037 在 benchmark/CLI 内部提供
`off`、`summary`、`trace` 三档性能模式：

- `off`：产品默认，不创建 recorder，不产生性能报告，也不改变提取语义。
- `summary`：按阶段聚合耗时、次数、吞吐、首帧/首条延迟和资源水位；逐帧数据只聚合，内存不随视频时长线性增长。
- `trace`：包含 summary，并额外记录逐字幕段成本；不默认记录每帧事件，避免 trace 失控。

性能数据必须与同次运行的 timing、CER、usable、noise、empty 一起输出。该 feature
只建立测量基础设施和固定负载 baseline，不包含 ROI 输出、代表帧裁剪、线程并发、
OCR 缓存或算法降采样；这些候选项在 baseline 完成后按耗时占比另立 feature。

## 2. 验收门

### 2.1 Benchmark 优化

- [x] 一条命令即可对「视频 + ground truth SRT」输出完整指标。
- [x] 报告包含 timing recall/precision/F1、边界误差、CER macro/micro、字符准确率、端到端可用召回、速度（× 实时）。
- [x] 报告输出 agent 可读 JSON 与逐条 GT/detection CSV，可直接定位漏检、合并、过切分、OCR 空文本和高 CER。
- [~] Benchmark 框架与最终固定 GT 报告已记录；feat-028 的独立“初始基线入库”经用户决定跳过，历史水位汇总见 `benchmark/README.md` 锚点表与 `docs/design/benchmark.md`。
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

### 2.5 开发者性能模式与 baseline（feat-037）

- [x] `off` / `summary` / `trace` 语义、配置和报告结构明确；产品 GUI 不增加用户开关。
- [x] summary 覆盖 probe、extractor 输出等待、crop/signature/changepoint、OCR、行选择/共识、finalize/dedupe 和总耗时。
- [x] trace 输出逐字幕段的代表帧数、OCR 次数、OCR/共识耗时和产出状态，不写入原始字幕文本。
- [x] 报告同时包含环境元数据、吞吐/延迟/资源指标和既有质量 gates。
- [x] 固定负载执行 1 次预热 + 3 次独立测量，以中位数形成版本化 baseline。
- [x] summary 相对 off 的三次运行中位耗时增幅目标 ≤ 5%；固定 GT 质量门不回退。

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
| **feat-035** | GUI 处理倍速显示 | gui-observability | feat-030 | 处理期间显示相对实时倍速；仅由真实进度计算；单测覆盖换算与无效输入 |
| **feat-036** | GUI 左栏自适应布局 | gui-layout | feat-030 | 预览不挤压下方交互；窄栏下提取控件和状态不换行溢出；单测覆盖尺寸计算 |
| **feat-037** | 开发者性能模式与性能基线 | performance-baseline | feat-027, feat-034 | 三档内部性能诊断；阶段/字幕段成本与质量联合报告；固定负载形成 baseline |

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
    │                               │
    │                               └─ feat-037 (性能模式 → 固定负载 baseline)
    │
    └─ feat-032 (文档收尾，依赖 030+031+033+034)
```

**原则**：先建 benchmark，再驱动打轴与 OCR 可用性；质量水位稳定后建立性能剖析，
再由 baseline 定义下一项性能机制优化。feat-037 不与具体优化混做，避免测量口径和被测对象同时变化。

## 5. 关键约束

- **允许修改 Phase 1 核心**：Phase 3 不再遵循 Phase 2 的「pipeline/ 零修改」约束，允许重构 `src/sublift/pipeline/` 和 `src/sublift/ipc/bridge.py` 以支持增量。
- **双端验证**：任何改动必须保持 Python 端 `./init.sh` 与 Swift 端 `swift test` 全绿。
- **不预先绑定实现**：本阶段只定目标和验收；具体技术方案在每个 feat 启动时评估。
- **进度显示要真实**：不允许假进度条，进度必须对应实际处理阶段。
- **性能模式仅供开发者**：不增加 GUI 用户选项；默认 `off`，benchmark baseline 显式使用 `summary`。
- **标签必须准确**：同步 extractor 中的等待包含解码、颜色转换与 stdout 输出，报告不得把它命名为纯 codec decode。
- **质量与性能同锚**：任何性能结论都必须附同次固定 GT 质量指标，不能用牺牲 precision/usable 的方式换速度。

## 6. 风险与权衡

| 风险 | 影响 | 缓解 |
|---|---|---|
| 增量处理重构面较大 | 可能引入回归 | 先用 benchmark 稳住基线，再小步重构 |
| 打轴优化与增量改动冲突 | 同时改 pipeline 结构 + 算法，bug 难定位 | 先打轴后增量，或分分支并行 |
| ground truth 素材不足 | benchmark 说服力不够 | 至少复用现有 Zootopia clip，并记录获取方式 |
| 首条反馈时间受 Vision 冷启动影响 | 优化效果有限 | 实测区分「server 启动」vs「算法首条」时间 |
| 显式 CJK 边界清理可能误删无空格英文 | `NPD动物警局`、`苹果的iPhone` 等混排受损 | 默认 `auto`；扩充混排 GT 后再收紧或移除启发式 |
| 长视频 GUI 体验尚未人工验收 | 自动指标不能覆盖进度观感与交互连续性 | 后续用 ≥10 分钟非 Zootopia 视频做拖拽、取消、重启验收 |
| 计时埋点本身扰动结果 | baseline 反映 profiler 而非产品 | 默认 off；summary 只做有界聚合；分别测 off/summary 中位数并设置 ≤5% 扰动目标 |
| Vision 冷启动与系统负载造成抖动 | 单次耗时不可复现 | 固定输入和配置；1 次预热 + 3 次独立测量；记录环境并用中位数、min/max 表示 |
| 同步 extractor 难拆“纯解码” | 错误归因可能驱动错误优化 | 报告为 extract wait/output；必要时再用 Instruments/sample 做 CPU 热点二级诊断 |
| 性能 baseline 仍依赖单一片源 | 不能代表所有字幕密度与片源 | 本 feature 只建立 canonical baseline；8/12fps 和长视频矩阵后续扩充，不冒充泛化结论 |

## 7. 阶段验收门（Phase 3 整体）

- [~] feat-027~028：benchmark 框架与固定 GT 报告完成；feat-028 独立基线入库按用户决定跳过。
- [x] feat-029~030 完成：增量处理跑通，CLI/GUI 真实进度与快速取消可用。
- [x] feat-031 完成：SSIM patrol 一阶落地（历史证据见 phase3.json）。
- [x] feat-033 完成：`timing_f1` 95.2%（≥ 95%）；precision 98.8%（相对 100% 基线 -1.2pp / 1 FA）。
- [x] feat-034 完成：usable 92.0%；noise 0；empty 0；CER 3.2%；F1 97.7%；precision 98.8%。
- [x] feat-032 完成：核心文档统一到 Phase 3 最终实现与固定 GT 口径；`./init.sh` 8/8 通过。
- [x] feat-035 完成：GUI 提取状态显示真实处理倍速（相对实时），不影响 IPC 与识别路径。
- [x] feat-036 完成：左栏预览保留控制区空间，提取栏两行布局并限制状态文本为单行。
- [x] feat-037 完成：off/summary/trace、归因/质量哈希/Queue 安全、干净 commit baseline（22.75×；OCR~41%/extract_wait~24%/materialize~15%；coverage 99.4%）、扰动 +0.03%。

## 8. feat-037 详细开发计划

### 8.1 设计边界

性能记录器作为可选诊断依赖注入公共 Python 路径。`Pipeline`、`FfmpegExtractor`、
OCR 引擎仍保持原有职责；它们只上报结构化 span/counter，不负责格式化报告。
benchmark 负责编排模式、重复运行、环境快照和联合报告。GUI 产品路径不感知
`PerformanceMode`，避免把内部诊断暴露为用户能力。

建议的数据流：

```text
manifest(performance.mode)
  → BenchmarkRun / PerformanceRecorder
      ├─ FfmpegExtractor spans + counters
      ├─ Pipeline feed/finalize spans
      ├─ OCR/line-select/consensus spans
      └─ SegmentTrace（仅 trace）
  → PerformanceSummary
  → agent JSON + summary Markdown + optional segment JSONL
  → 与 timing/CER/usable gates 联合归档
```

### 8.2 计时与指标定义

统一使用 `time.perf_counter_ns()`；毫秒只在序列化层转换。当前串行路径的各阶段可以求和，
未来若引入并发则同时保留 wall span，禁止把重叠阶段简单相加为总耗时。

| 层级 | 必需指标 | 口径 |
|---|---|---|
| benchmark | `benchmark_wall_ms` | 从本次运行初始化到结果就绪；报告写盘另计 |
| core | `core_wall_ms`, `realtime_factor` | `Pipeline.run()` 的 wall time；视频时长 / core wall time |
| probe | `probe_ms` | ffprobe 尺寸与时长探测 |
| extractor | `spawn_to_first_frame_ms`, `extract_wait_ms`, `frame_count`, `raw_output_bytes` | 等待包含 codec decode、filter、RGB 转换与 stdout 输出，不称“纯解码” |
| frame pipeline | crop/颜色转换、signature、changepoint 的 count/total/mean/max | summary 只存聚合分布，不输出逐帧记录 |
| OCR | `ocr_calls`, total/mean/p50/p95/max | Vision `recognize()` 的 wall time；重试和多代表帧分别计次 |
| text selection | line-select、cleanup、consensus 的 count/total | 与 Vision 调用分开，避免把选择算法误算成 OCR |
| finalize | 尾段 OCR、dedupe、最终整理耗时 | 尾段 OCR 同时进入 OCR 汇总，finalize 保留 wall span |
| latency | `first_frame_ms`, `first_entry_ms` | 相对 core 开始；无字幕时为 null，不伪造 0 |
| resources | Python peak RSS、user/system CPU seconds | 明确采样方法与平台单位；每次测量独立进程以隔离峰值 |

逐字幕段 trace 至少包含：`start_ms`、`end_ms`、`duration_ms`、代表帧数、OCR 调用次数、
OCR wall time、选择/共识 wall time、是否接受、输出字符数。默认不写原始字幕文本，兼顾隐私和报告体积。

### 8.3 模式与开销控制

| 模式 | 行为 | 适用场景 |
|---|---|---|
| `off` | 不创建 recorder；热点路径只保留一次空值判断 | CLI/GUI 默认、质量回归 |
| `summary` | 聚合阶段 span/counter；分布使用固定桶或有界样本 | 正式性能 baseline、优化前后比较 |
| `trace` | summary + 逐字幕段 JSONL | 定位异常慢段、OCR 重试和共识成本 |

summary 不保存完整逐帧 duration 列表；若需要 p50/p95，使用固定桶或固定上限的确定性样本。
trace 文件按行增量写入，任务取消或异常时也应保持已完成记录可解析，并在 summary 标注
`completed=false` 和终止原因。

### 8.4 报告契约

现有 `.agent.json` 增加顶层 `performance`，现有 `.summary.md` 增加性能总表；质量字段和
failure clusters 不改名。trace 模式额外生成 `.perf-segments.jsonl`。建议结构：

```json
{
  "performance": {
    "schema_version": 1,
    "mode": "summary",
    "completed": true,
    "environment": {},
    "workload": {},
    "latency": {},
    "throughput": {},
    "resources": {},
    "stages": {},
    "runs": [],
    "aggregate": {"primary": "median", "measured_runs": 3}
  }
}
```

环境至少记录 commit、dirty 状态、macOS/架构、CPU、Python、Vision/PyObjC、ffmpeg 版本；
负载至少记录视频路径或稳定标识、尺寸、时长、fps、region、profile、关键 Config。性能数据
只在同环境/同负载下直接比较；跨机器结果只做参考。

### 8.5 Canonical baseline 协议

固定负载沿用质量锚，不新造 Mock 长流作为主 baseline：

```text
video: Zootopia_clip_1080p.mkv
ground truth: benchmark/datasets/Zootopia_clip_1080p_gt.srt
resolution: 1920×1080
fps: 5
engine: Vision
region: [0, 848, 1920, 87]
subtitle_script: cjk
runs: warmup=1（丢弃）, measured=3（独立进程）
primary statistic: median；同时记录 min/max
```

baseline 必须记录 core wall、实时倍速、首帧/首条延迟、各阶段占比、OCR 次数与分布、
raw output bytes、Python peak RSS 和 CPU seconds。每个 measured run 都要计算质量指标并过门；
最终数值与环境写入 `benchmark/README.md` 锚点表 / `docs/design/benchmark.md`。5fps 是完成门，8/12fps 仅用于后续观察
采样率扩展曲线。

### 8.6 测试与验收顺序

1. 用 fake clock 单测 recorder：嵌套 span、异常关闭、计数、聚合、序列化和有界样本。
2. 单测 manifest：非法 mode/repeat 拒绝，默认 off 不改变旧 manifest 行为。
3. 用 Mock OCR 短视频跑 off/summary/trace，断言字幕结果一致、summary 字段完整、trace 可逐行解析。
4. 验证取消/异常仍能产出 `completed=false` 的部分 summary，且不妨碍资源回收。
5. 同负载各跑 off/summary 三次，summary 中位耗时增幅目标 ≤5%。
6. 按 canonical baseline 协议跑 Vision：1 次预热 + 3 次正式测量，记录中位数和离散度。
7. 确认每次质量门：timing F1≥95.2%、precision≥98.8%、usable≥85.1%、CER≤6.6%、noise≤2、empty≤1。
8. 运行 `uv run ruff check .`、`uv run mypy src tests`、`uv run pytest` 和 `./init.sh`，将证据写回 `docs/phases/phase3.json`。

### 8.7 完成定义

feat-037 只有同时满足以下条件才可改为 `done`：

- 三档模式、阶段埋点、逐字幕段 trace 和联合报告已实现且有测试。
- 默认 off 对现有 CLI/GUI、Pipeline 结果和协议无行为改变。
- summary 开销达到目标，或有充分证据解释并由用户接受新的阈值。
- canonical baseline 已真实运行，环境、命令、3 次结果、中位数和质量门已归档。
- 没有把 profiler 输出误称为纯 codec decode，也没有在本 feature 偷做具体性能优化。
- 标准验证全绿，phase3.json 保存验证证据，progress.md 保持精简。
