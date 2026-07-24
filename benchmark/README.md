# SubLift Benchmark 用法

端到端质量诊断与开发者性能剖析。  
**设计**见 [docs/design/benchmark.md](../docs/design/benchmark.md)；**架构**见 [docs/ARCHITECTURE.md](../docs/ARCHITECTURE.md) §11。

## 前置

| 依赖 | 说明 |
|---|---|
| Python 3.12+ / uv | `./init.sh` 或 `uv sync --extra vision` |
| ffmpeg / ffprobe | PATH 可用 |
| 视频 | 默认固定片段：`debug/Zootopia_clip_1080p.mp4`（**不入版本控制**，约 78MB） |
| GT | `benchmark/fixtures/Zootopia_clip_1080p_gt.srt`（入库） |

无视频时无法本地复现固定 GT / 性能 baseline。

## 一键运行

```bash
# 质量路径示例（下部裁剪，无性能块）
uv run --extra vision python scripts/run_benchmark_manifest.py \
  benchmark/manifests/zootopia_cli_bottomcrop_5fps.json \
  --label manifest

# 固定区域 + 性能 summary（1 预热 + 3 次测量）
uv run --extra vision python scripts/run_benchmark_manifest.py \
  benchmark/manifests/zootopia_fixed_region_5fps_perf.json \
  --label feat037_perf_baseline
```

| `--label` | 行为 |
|---|---|
| `auto`（默认） | 按最近 commit 主题生成递增子目录 |
| `manifest` | 使用 manifest 内 `label`，并写入 `output_dir/<label>/` |
| 其它字符串 | 固定 label，输出到 `output_dir/<label>/` |

## Manifest 字段

相对路径相对于**仓库根**。

| 字段 | 类型 | 默认 | 说明 |
|---|---|---|---|
| `video` / `video_path` | string | 必填 | 输入视频 |
| `ground_truth` / `ground_truth_path` | string | 必填 | GT SRT |
| `fps` | number | 5.0 | 采样率 |
| `engine` | string | `"vision"` | `vision` / `mock` |
| `confidence` | number | 0.5 | OCR 置信门 |
| `subtitle_script` | string | `"auto"` | `auto` / `cjk` / `latin` |
| `match_threshold` | number | 0.5 | temporal IoU 匹配阈值 |
| `region_box` | `[x,y,w,h]` 或 null | null | 固定区；null → 下部裁剪 |
| `label` | string | null | 报告后缀 |
| `output_dir` | string | `debug/benchmark-reports` | 报告根目录（perf manifest 现为 `debug/perf_reports`） |
| `video_duration_seconds` | number | null | 覆盖探测时长 |
| `performance` | object | 省略=off | 见下表 |

### `performance` 对象

| 字段 | 默认 | 说明 |
|---|---|---|
| `mode` | `"off"` | `off` / `summary` / `trace` |
| `warmup_runs` | 0 | 预热次数（丢弃） |
| `measured_runs` | 1 | 计入中位数的次数 |

示例（性能 baseline 配方）：

```json
{
  "video": "debug/Zootopia_clip_1080p.mp4",
  "ground_truth": "benchmark/fixtures/Zootopia_clip_1080p_gt.srt",
  "fps": 5.0,
  "engine": "vision",
  "confidence": 0.5,
  "subtitle_script": "cjk",
  "match_threshold": 0.5,
  "region_box": [0, 848, 1920, 87],
  "label": "feat037_perf_baseline",
  "output_dir": "debug/perf_reports",
  "performance": {
    "mode": "summary",
    "warmup_runs": 1,
    "measured_runs": 3
  }
}
```

## 产物

每次成功运行在输出目录生成：

| 文件 | 内容 |
|---|---|
| `*.agent.json` | 完整诊断 + 可选 `performance` |
| `*.summary.md` | 人读摘要 |
| `*.gt_cases.csv` | 每条 GT |
| `*.det_cases.csv` | 每条 detection |
| `*.perf-segments/runN.jsonl` | 仅 `trace`：逐段耗时（无字幕原文） |

建议阅读顺序：`summary.md` → failure clusters → CSV →（可选）`performance.stages`。

## Python API

```python
from pathlib import Path
from benchmark import RunConfig, run_benchmark, align_existing_srt
from benchmark.report import write_reports

cfg = RunConfig(
    video_path=Path("debug/Zootopia_clip_1080p.mp4"),
    ground_truth_path=Path("benchmark/fixtures/Zootopia_clip_1080p_gt.srt"),
    fps=5.0,
    engine="vision",
    subtitle_script="cjk",
    region_box=(0, 848, 1920, 87),
    label="my_run",
    output_dir=Path("debug/perf_reports/my_run"),
    performance_mode="summary",  # off | summary | trace
    warmup_runs=0,
    measured_runs=1,
)
write_reports(run_benchmark(cfg))

# 仅对齐已有 SRT（不跑提取）
# write_reports(align_existing_srt(cfg, Path("debug/some_export.srt")))
```

## 性能模式速查

| 模式 | 何时用 |
|---|---|
| `off` | 日常质量回归、对比算法（默认） |
| `summary` | 正式性能 baseline、优化前后对比 |
| `trace` | 定位最慢字幕段 / OCR 次数 |

阶段名与口径（`extract_wait` **含** decode+filter+RGB+pipe，**不是**纯解码）见设计文档 §6。

summary 扰动自测：

```bash
uv run --extra vision python scripts/measure_perf_overhead.py
# 默认写 debug/perf_reports/feat037_overhead/overhead.json
```

对比优化时同环境同负载，读 `.agent.json` 的：

- `performance.aggregate.core_wall_ms` / `realtime_factor`
- `performance.throughput.raw_output_bytes` / `unattributed_ms` / `stage_coverage_pct`
- `performance.stages.*.total_ms`
- `performance.quality`（三次 hash 与 gates）

## 当前回归锚点

> 固定 Zootopia 片段；**不**代表跨片源泛化。已验收的质量与当前 ROI 性能结论见
> [版本化基线报告](reports/README.md)。`debug/` 只保存本地重跑原始产物，不能自动取代
> 已提交的基线快照。

### 素材

| 项 | 值 |
|---|---|
| 视频 | `debug/Zootopia_clip_1080p.mp4`（1080p，~254.27s） |
| GT | `benchmark/fixtures/Zootopia_clip_1080p_gt.srt`（87 条） |

### 质量（feat-034 P1 fix2）

配置要点：5fps、Vision、region 精准对齐、`subtitle_script=cjk`、行级选择开启。

| 指标 | 锚点 |
|---|---:|
| timing_recall | 96.6% |
| timing_precision | 98.8% |
| timing_f1 | **97.7%** |
| cer_macro | **3.2%** |
| usable_subtitle_recall | **92.0%** |
| text.noise / text.empty | **0 / 0** |

版本化结论：[固定 GT 质量基线](reports/quality-baseline.md)。原始历史产物仍在本地
`debug/benchmark-reports/feat034_p1_fix2/`。

### 性能协议与量级（feat-037）

| 项 | 值 |
|---|---|
| 配置 | 5fps、Vision、region `[0,848,1920,87]`、cjk |
| 运行 | warmup=1 + measured=3（独立进程） |
| 主统计 | median（附 min/max） |
| manifest | `benchmark/manifests/zootopia_fixed_region_5fps_perf.json` |
| 默认输出 | `debug/perf_reports/` |

干净树一次归档量级（commit 期实测，供对照）：core 中位约 **11.2 s**（~**22.8×** 实时）；OCR ~41%、`extract_wait` ~24%、`frame_materialize` ~15%；`raw_output_bytes` ≈ **7.91 GB（7.36 GiB）**/ 次；阶段 coverage ~99%。  
上述 feat-037 数字是 ROI 前的历史性能模式基线。当前默认 ROI 路径的 clean-commit A/B、
完整 coverage 与阶段归因见[性能归因基线](reports/performance-attribution-baseline.md)；本地
重跑仍输出到 `debug/`。

### 历史备注（非现行验收主口径）

| 阶段 | timing F1 | 说明 |
|---|---:|---|
| baseline-no-filter | 91.2% | patrol on，hyst=2 |
| feat-033 final | 95.2% | 过 95% 门 |
| feat-034 P1 fix2 | **97.7%** | 当前质量锚点 |

早期 CLI bottom-crop vs GUI 固定区 CER 反差说明：**区域宽度是 OCR 问题**，不是打轴主矛盾。

## feat-039：同提交 full / roi A/B

在同一 clean commit、同机器上对照内部 `frame_output_mode`：

```bash
uv run python scripts/run_benchmark_manifest.py \
  benchmark/manifests/zootopia_feat039_full.json --label manifest
uv run python scripts/run_benchmark_manifest.py \
  benchmark/manifests/zootopia_feat039_roi.json --label manifest
uv run python scripts/compare_roi_ab.py \
  debug/benchmark-reports/feat039_roi_ab/feat039_full/Zootopia_clip_1080p_feat039_full.agent.json \
  debug/benchmark-reports/feat039_roi_ab/feat039_roi/Zootopia_clip_1080p_feat039_roi.agent.json \
  --out debug/benchmark-reports/feat039_roi_ab/ab_summary.json
```

| 项 | 固定值 |
|---|---|
| 负载 | 与 feat-037 canonical 相同（Zootopia 1080p / 5fps / region `[0,848,1920,87]` / Vision / cjk） |
| 协议 | 每组 warmup=1 + measured=3，主统计 median |
| 硬门 | hash 等价、raw bytes 比例 87/1080、materialize ≤25% full、wall/RSS ≤105% full、coverage ≥99%、固定 GT 质量门 |
| 输出 | `debug/benchmark-reports/feat039_roi_ab/`（不入库；证据写入 `docs/phases/phase4.json`） |

`frame_output_mode` 默认 `full`；`roi` 需要 `region_box`。结论只能同提交同机比较，不能与跨机器历史数字混比。

## 相关路径

| 路径 | 说明 |
|---|---|
| [docs/design/benchmark.md](../docs/design/benchmark.md) | 模块、匹配、指标、性能模式设计 |
| `benchmark/manifests/` | 可复现 JSON 配方 |
| `benchmark/fixtures/` | GT SRT |
| `scripts/run_benchmark_manifest.py` | CLI 入口 |
| `scripts/compare_roi_ab.py` | feat-039 full/roi 硬门对照 |
| `debug/perf_reports/` | 当前性能报告默认根 |
| `debug/benchmark-reports/` | 历史质量/混合报告根 |
