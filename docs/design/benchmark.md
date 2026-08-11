# Benchmark v2 设计

> 实现：`src/sublift/benchmark/`
> 配置与数据：`benchmark/`
> 固定本地媒体：`debug/`；本机运行产物：`debug/benchmark/`
> 入口：`uv run sublift-benchmark`

用法见 [`benchmark/README.md`](../../benchmark/README.md)。

## 1. 目标

- 用一个入口覆盖单组运行、参数矩阵、已有 SRT 评分与专项比较；
- 代码、配置、数据、冻结基线、临时产物有明确边界；
- 新增参数时复用通用 `--set / --vary`，不继续新增一次性扫描脚本；
- 质量、性能和外部导出共享一套 SRT 对齐与报告模型；
- 保留历史 flat manifest 与旧脚本包装器的迁移窗口。

## 2. 分层

```text
┌─────────────────────────────────────────────────────────┐
│ CLI                                                     │
│ run · matrix · score · show · overhead · compare-roi   │
└───────────────────────┬─────────────────────────────────┘
                        │
┌───────────────────────▼─────────────────────────────────┐
│ Config / Matrix                                         │
│ v1/v2 JSON · dotted override · Cartesian product        │
└───────────────┬───────────────────────┬─────────────────┘
                │                       │
┌───────────────▼──────────────┐  ┌────▼─────────────────┐
│ Execution                    │  │ Existing SRT scoring │
│ Python Pipeline + perf spans │  │ runtime agnostic     │
└───────────────┬──────────────┘  └────┬─────────────────┘
                └──────────────┬────────┘
                               │
┌──────────────────────────────▼──────────────────────────┐
│ Diagnostics / Reports                                  │
│ temporal IoU · CER · usable · cases · JSON/CSV/MD      │
└─────────────────────────────────────────────────────────┘
```

依赖方向是 CLI → config/matrix → runner → diagnostics/report。配置层不依赖 runner；
`RunConfig` 定义在 `config.py`，避免旧结构中 manifest 为了得到配置类型反向依赖执行层。

## 3. 模块职责

| 模块 | 职责 |
|---|---|
| `cli.py` | 唯一命令入口和输出目录策略 |
| `config.py` | `RunConfig`、v1/v2 JSON、路径解析、未知字段拒绝、dotted override |
| `pipeline_config.py` | Pipeline 参数白名单、嵌套类型校验与不可变 Config 合成 |
| `matrix.py` | `--set / --vary` 解析、组合数上限、确定性 label、笛卡尔积 |
| `runner.py` | Python Pipeline 执行、warmup/measured、进程隔离、质量快照 |
| `srt.py` | GT 与 detection 共用的 SRT 解析 |
| `diagnostics.py` | 一对一对齐、指标、case 分类、gates、failure clusters |
| `report.py` | 单组 agent JSON、GT/det CSV、Markdown |
| `matrix_report.py` | 矩阵 plan、聚合 JSON/CSV/Markdown |
| `overhead.py` | 交错 off/summary 配对与输出 hash 一致性 |
| `roi_compare.py` | feat-039 full/ROI 冻结硬门 |
| `git_utils.py` | 显式 `--label auto` 的递增目录 |

产品热路径仍只依赖 `src/sublift/diagnostics/performance.py` 中的可选 recorder，
不依赖 benchmark 包。

## 4. 资产与兼容生命周期

| 类别 | 路径 / 入口 | 生命周期与规则 |
|---|---|---|
| Canonical CLI | `uv run sublift-benchmark` | 唯一受支持的 benchmark 入口；活跃文档、自动化与新命令只使用它。 |
| Historical shim | `scripts/run_benchmark_manifest.py`、`scripts/measure_perf_overhead.py`、`scripts/compare_roi_ab.py` | R2 历史复现兼容面；只可打印提示并向 canonical CLI 转发，不复制 runner/config/report 实现。由 `tests/test_benchmark_wrappers.py` 覆盖。 |
| Root marker compatibility | `phases.json`；`feature-list.json` fallback | 新 checkout 优先使用 `phases.json`；仅 legacy checkout 使用 `feature-list.json`。无任一 marker 时回退调用时 cwd，三种情况由 `tests/test_benchmark_config.py` 覆盖。 |
| Versioned configs | `benchmark/configs/` | 可复现 run/matrix 配置，入库；被报告或测试引用的 Phase config 不因编号或名称过旧而删除。 |
| Versioned datasets | `benchmark/datasets/` | GT、生成 recipe 与数据 manifest，入库。 |
| Versioned baselines | `benchmark/baselines/` | 已验收冻结结论，入库；临时运行不得写入。 |
| Versioned parity assets | `benchmark/parity/` | C++ cutover fixtures/goldens，入库；不是运行垃圾。 |
| Local inputs | `debug/Zootopia_*.mp4|mkv` | 历史配置与专项脚本引用的固定本地媒体，不入库；不是临时运行产物。 |
| Local generated artifacts | `debug/benchmark/imports/`、`runs/`、`perf/`、`archive/` | 机器本地产物；分别承载导入、当前运行/矩阵报告、性能运行和旧目录迁移归档。 |

临时运行不能写入 `benchmark/baselines/`。基线晋升必须先完成同负载、多轮、质量门
和环境记录，再手工复制结论。

## 5. Config v2

```json
{
  "schema_version": 2,
  "run": {
    "video": "debug/video.mp4",
    "ground_truth": "benchmark/datasets/video_gt.srt",
    "fps": 5,
    "engine": "vision",
    "pipeline": {
      "min_duration_ms": 300,
      "change_point": {"hysteresis_frames": 1}
    },
    "output_dir": "debug/benchmark/runs",
    "performance": {
      "mode": "off",
      "warmup_runs": 0,
      "measured_runs": 1
    }
  },
  "matrix": {
    "fps": [5, 8, 12],
    "engine": ["vision", "paddle"],
    "pipeline.change_point.presence_threshold": [0.01, 0.02]
  }
}
```

解析顺序：

1. 读取 v1 flat 或 v2 `run`；
2. 从配置文件向上发现仓库根；
3. 应用 `--set`；
4. matrix 时将 config/CLI 轴取笛卡尔积；
5. 每个 cell 重新经过完整类型、范围、组合约束验证；
6. 生成确定性 cell label 与独立输出目录。

未知字段直接失败，避免拼写错误静默回退默认值。矩阵默认最多 64 组，防止误写
多个大轴造成意外长跑。

Pipeline 调参统一放在 `run.pipeline` 下，并能用 dotted path 进入 matrix。
允许字段从 `Config / SignatureConfig / ChangePointConfig` dataclass 派生；
`sample_fps / confidence_threshold / subtitle_profile / subtitle_script` 保留给 run 层，
避免同一含义出现两个来源。新增普通 Pipeline dataclass 字段后，benchmark
不需要再增加 CLI 开关。

## 6. 执行与评分

### 6.1 `run / matrix / overhead`

使用 Python in-process 产品组件：

```text
FfmpegExtractor
  → FixedRegion / BottomCrop / RoiPassthrough
  → Pipeline
  → Vision / Paddle / Mock OCR
  → SRT entries
  → diagnostics
```

原因是 performance recorder 目前位于 Python 产品路径，能提供阶段 wall、RSS、CPU、
raw bytes、OCR calls 与 trace。多次 measured 默认 spawn 隔离峰值 RSS。

### 6.2 `score`

只读取已有 SRT，不执行提取。它不关心导出来自 C++、Python、GUI 或第三方工具，因此
是 runtime parity 和人工导出回归的统一入口。外部日志中的 wall/duration 可以显式传入。

原生 C++ 内部性能仍使用 cutover/Paddle 专项门；在 C++ recorder 与 Python recorder
形成同构 schema 前，不伪造阶段级横向比较。

## 7. 参数矩阵

`--set` 固定所有 cell 的参数，`--vary` 定义轴。CLI 轴覆盖 config 中同名轴。

每个矩阵先形成只读 plan：

```text
base run
  + fixed overrides
  × axis 1
  × axis 2
  ...
  → validated MatrixCell[]
```

非 dry-run 会先写 `matrix.plan.json`，再逐 cell 运行。默认 fail-fast；`--keep-going`
允许收集全部错误。聚合报告只引用每个 cell 的标准产物，不另算一套指标。

## 8. 诊断口径

一对一时间匹配：

1. 计算 det×GT temporal IoU；
2. IoU 降序贪心；
3. 默认 `IoU >= 0.5`；
4. 每条 GT/detection 最多匹配一次。

指标：

| 维度 | 指标 |
|---|---|
| timing | recall、precision、F1、start/end MAE、boundary P95 |
| recognition | CER macro/micro、char accuracy、exact match |
| e2e | usable subtitle recall（时间命中且 CER≤20%） |
| speed | elapsed seconds、realtime factor |

case 分类保留 `fn.no_overlap / merged / boundary_miss / false_alarm / split_extra /
text.empty / high_cer / noise`。矩阵聚合不替代单条 case CSV。

## 9. 性能模式

| 模式 | 行为 |
|---|---|
| `off` | 不创建 recorder |
| `summary` | 有界阶段聚合 |
| `trace` | summary + 无原文的逐段 JSONL |

coverage 叶子仍为 probe、extract_wait、frame_materialize、crop、color_convert、
signature、changepoint、ocr、line_select、cleanup、consensus、dedupe 和
pipeline_overhead；`finalize` 是容器 span，不重复计入。

`overhead` 采用 off/summary 交错配对，并同时检查全部输出 hash 一致；性能扰动不能以
结果漂移换取。

## 10. 兼容与后续扩展

- `scripts/run_benchmark_manifest.py`、`measure_perf_overhead.py`、
  `compare_roi_ab.py` 只做转发并提示新命令；
- 历史 flat manifest 继续解析；
- 算法专项 trace/短字幕脚本归入 `scripts/diagnostics/`，不再冒充标准 benchmark；
- 新运行字段只需在 `RunConfig + config parser` 注册；新 Pipeline dataclass 标量字段
  自动进入 `pipeline.*` 白名单；CLI 和 matrix 都无需增加专用开关；
- 若将来 C++ 暴露同构 performance schema，可新增 execution backend，而不改变
  config/matrix/diagnostics/report 层。
