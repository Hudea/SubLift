# SubLift Benchmark

> Phase 13 将本目录定位为隔离、可选、离线的工程工具与版本化资产；它不是产品
> 运行时，也不得成为 Native 产品门禁的必需依赖。`run/matrix/overhead` 当前仍重放
> 历史 Python Pipeline；`score` 只读取已有 SRT，与产生它的 runtime 无关。

可选的端到端字幕质量诊断、参数矩阵、已有 SRT 评分与历史 Python Pipeline 性能归因。

统一入口：

```bash
uv run sublift-benchmark --help
```

设计见 [`docs/design/benchmark.md`](../docs/design/benchmark.md)。

## 目录边界

```text
src/sublift/benchmark/       # 可执行代码（随 sublift 包安装）
benchmark/
  configs/                   # 可复现 run / matrix JSON
  datasets/                  # GT、确定性生成 recipe 与数据 manifest
  baselines/                 # 已验收、进入版本控制的冻结结论
  parity/                    # C++ cutover fixtures / goldens
debug/                       # 配置引用的固定本地媒体（不入库）
debug/benchmark/             # imports、runs、perf 与历史归档（不入库）
scripts/
  run_benchmark_manifest.py  # 三个历史复现 shim 之一
  measure_perf_overhead.py   # 三个历史复现 shim 之一
  compare_roi_ab.py          # 三个历史复现 shim 之一
  diagnostics/               # 非标准算法定位脚本
```

原则：

- 代码不放在仓库根 `benchmark/`，避免“Python 包、数据、报告”同层混放；
- 临时运行一律进入 `debug/benchmark/`；
- `debug/Zootopia_*.mp4|mkv` 是历史版本化配置引用的本地输入契约，不是运行产物；
- `benchmark/baselines/` 只保存经过协议验收的版本化结论；
- Native CLI/GUI/Server、历史 Python 或外部工具导出均可通过同一个 `score` 口径比较。

`phases.json` 是当前 checkout 的 root marker；`feature-list.json` 只为旧 checkout
保留 fallback。`configs / datasets / baselines / parity` 都是版本化资产，不属于本机清理
候选；本机输入和生成物的边界见 [`docs/design/benchmark.md`](../docs/design/benchmark.md)。

## 四个可选工具命令

### 1. 执行单组配置

```bash
uv run sublift-benchmark run \
  benchmark/configs/zootopia_fixed_region_5fps_perf.json
```

临时覆盖参数，不需要复制 JSON：

```bash
uv run sublift-benchmark run \
  benchmark/configs/zootopia_fixed_region_5fps_perf.json \
  --set fps=8 \
  --set engine=paddle \
  --set performance.mode=off \
  --label paddle_8fps
```

`run` 当前使用过渡期 Python in-process Pipeline 重放历史阶段性能埋点；支持
`vision / paddle / mock`。它不是产品提取入口；Native 或 GUI 已有导出使用 `score`。

### 2. 参数矩阵

配置内可声明 matrix，也可在命令行追加/替换参数轴：

```bash
uv run sublift-benchmark matrix \
  benchmark/configs/zootopia_fps_engine_matrix.json \
  --dry-run

uv run sublift-benchmark matrix \
  benchmark/configs/zootopia_fixed_region_5fps_perf.json \
  --set performance.mode=off \
  --set performance.warmup_runs=0 \
  --set performance.measured_runs=1 \
  --vary 'fps=[5,8,12]' \
  --vary 'engine=["vision","paddle"]'
```

矩阵规则：

- 多个轴取笛卡尔积；
- `--set` 是所有 cell 的固定覆盖；
- `--vary` 覆盖 config 中同名 matrix 轴；
- 默认最多 64 组，可用 `--max-cases` 显式提高；
- 默认 fail-fast；需要收集全部失败时使用 `--keep-going`；
- `--dry-run` 只打印最终配置，不写文件、不跑视频。

每次矩阵额外生成：

- `matrix.plan.json`
- `matrix.results.json`
- `matrix.results.csv`
- `matrix.summary.md`

### 3. 评分已有 SRT

适用于 Native CLI/GUI/Server、历史 Python 或外部工具的导出：

```bash
uv run sublift-benchmark score \
  benchmark/configs/zootopia_fps_engine_matrix.json \
  debug/benchmark/imports/2026-07-30/cpp/export.srt \
  --set fps=8 \
  --set engine=vision \
  --elapsed 10.79 \
  --duration 254.25 \
  --label vision_cpp_8fps
```

`score` 不重新提取视频，只执行 GT 对齐、指标、failure clusters 和标准报告。

### 4. 查看最终配置

```bash
uv run sublift-benchmark show \
  benchmark/configs/zootopia_fps_engine_matrix.json \
  --set fps=12
```

适合在长跑前确认路径、ROI、performance 和 CLI override 是否生效。

## 专项命令

### Performance recorder 扰动

交错运行 off/summary 配对，并同时验证输出 hash：

```bash
uv run sublift-benchmark overhead \
  benchmark/configs/zootopia_fixed_region_5fps_perf.json \
  --runs 3 \
  --target-pct 5 \
  --label recorder_overhead
```

### Full / ROI 硬门比较

```bash
uv run sublift-benchmark compare-roi \
  path/to/full.agent.json \
  path/to/roi.agent.json \
  --out debug/benchmark/runs/roi_ab/ab_summary.json
```

## Config v2

推荐结构：

```json
{
  "schema_version": 2,
  "run": {
    "video": "debug/Zootopia_clip_1080p.mp4",
    "ground_truth": "benchmark/datasets/Zootopia_clip_1080p_gt.srt",
    "fps": 5.0,
    "engine": "vision",
    "confidence": 0.5,
    "subtitle_script": "cjk",
    "match_threshold": 0.5,
    "region_box": [0, 848, 1920, 87],
    "frame_output_mode": "roi",
    "pipeline": {
      "min_duration_ms": 300,
      "change_point": {
        "hysteresis_frames": 1
      }
    },
    "label": "zootopia",
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

历史 flat manifest 继续兼容。新配置会拒绝未知字段，避免把 `fps` 拼成 `fpps`
后静默使用默认值。

### 可覆盖字段

| 字段 | 说明 |
|---|---|
| `video` / `ground_truth` | 视频与 GT 路径；相对仓库根 |
| `fps` | 采样率，必须 >0 |
| `engine` | `vision / paddle / mock` |
| `confidence` | OCR 置信门，0–1 |
| `subtitle_script` | `auto / cjk / latin` |
| `match_threshold` | temporal IoU 门，0–1 |
| `region_box` | `[x,y,width,height]` 或 null |
| `frame_output_mode` | `full / roi`；roi 需要 region |
| `pipeline.*` | Pipeline 算法参数；支持嵌套 `signature.* / change_point.*` |
| `label` / `output_dir` | 报告身份与输出根 |
| `video_duration_seconds` | 覆盖探测到的视频时长 |
| `isolate_processes` | 多次测量是否 spawn 隔离 |
| `performance.mode` | `off / summary / trace` |
| `performance.warmup_runs` | 丢弃的预热次数 |
| `performance.measured_runs` | 计入聚合的次数 |

CLI 值优先按 JSON 解析，因此 boolean 要写 `true/false`，数组写 `[1,2,3]`。
例如可直接扫描变化点迟滞，不需要新增脚本：

```bash
uv run sublift-benchmark matrix benchmark/configs/zootopia_fps_engine_matrix.json \
  --vary 'pipeline.change_point.hysteresis_frames=[1,2,3]' \
  --set pipeline.min_duration_ms=300 \
  --dry-run
```

`fps / confidence / subtitle_script` 仍由 run 顶层统一管理，不能在
`pipeline` 中重复覆盖。未知或类型不匹配的 Pipeline 字段会在长跑前失败。

## 标准报告

每个 run/score cell 输出：

| 文件 | 内容 |
|---|---|
| `*.agent.json` | 完整配置、gates、指标、cases、failure clusters |
| `*.summary.md` | 人读摘要 |
| `*.gt_cases.csv` | 每条 GT 的匹配、CER 和失败类型 |
| `*.det_cases.csv` | 每条 detection 的匹配结果 |
| `*.perf-segments/*.jsonl` | 仅 trace；逐段耗时，无字幕原文 |

建议阅读顺序：summary → matrix summary → failure clusters → GT/det CSV → trace。

## 指标

- timing：recall / precision / F1、start/end MAE、boundary P95；
- recognition：CER macro/micro、char accuracy、exact match；
- e2e：usable subtitle recall（时间命中且 CER≤20%）；
- speed：wall 与 realtime factor；
- performance：阶段 wall、首帧/首条、帧数、raw bytes、OCR calls、RSS、CPU。

统一匹配是一对一 temporal IoU，默认阈值 0.5。

## 固定回归锚

Zootopia 1080p / 5fps / Vision / ROI `[0,848,1920,87]`：

| 指标 | 锚点 |
|---|---:|
| timing F1 | 97.7% |
| timing precision | 98.8% |
| CER macro | 3.2% |
| usable recall | 92.0% |
| noise / empty | 0 / 0 |

详见：

- [`baselines/quality-baseline.md`](baselines/quality-baseline.md)
- [`baselines/performance-attribution-baseline.md`](baselines/performance-attribution-baseline.md)

该单一片源只用于回归，不代表跨片源泛化。

## 兼容入口

以下旧脚本是历史复现 shim：仍可运行，但只负责转发并打印迁移提示。新自动化和文档不得
调用它们。

```bash
uv run python scripts/run_benchmark_manifest.py <config>
uv run python scripts/measure_perf_overhead.py
uv run python scripts/compare_roi_ab.py <full> <roi>
```

唯一 canonical 入口是 `uv run sublift-benchmark`。
