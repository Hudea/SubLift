# SubLift Benchmark 基线

> 本文件记录当前 benchmark 基线，作为后续优化的对比依据。
> feat-027 已统一为诊断报告口径；feat-028 会重新录入当前正式基线。

## 素材

| 项 | 值 |
|---|---|
| 视频片段 | `debug/Zootopia_clip_1080p.mp4`（1080p, 254.27s, H.264） |
| Ground truth | `benchmark/fixtures/Zootopia_clip_1080p_gt.srt`（87 条，从整集字幕 `debug/Zootopia_cn.srt` 切前 254s 得到） |

> 视频 78MB 不入版本控制；GT 文本入库。素材不在库时无法本地复现。

## 历史参考

以下数值来自旧 benchmark 口径，仅用于理解问题来源，不作为后续验收主口径：

| 配置 | 旧打轴 R | 旧打轴 P | 旧打轴 F1 | 旧 CER | 空文本 | 速度 |
|---|---|---|---|---|---|---|
| CLI 默认 (BottomCrop 30%, 5fps, vision) | 78.2% | 81.0% | 79.5% | 88.0% | 13/74 | 17.5× |
| GUI 历史 (选定区域, 8fps, vision) | 64.4% | 94.9% | 76.7% | 45.8% | 0/59 | N/A |

## 关键发现

两条路径呈现相反偏差，反映两个独立问题：

### 1. 区域裁剪宽度影响 CER（非打轴问题）

- **CLI 路径**用 `BottomCropDetector(bottom_ratio=0.3)`，裁剪过宽吃进画面噪声，导致 OCR 空文本 13 条、CER 88%。
- **GUI 路径**用用户在 Vision 候选框中多选合并的 `FixedRegionDetector`，区域精准，CER 降到 45.8%、无空文本。
- 与 `docs/ARCHITECTURE.md` §9 已知限制一致：**区域裁剪优化属 OCR 问题，Phase 3 明确后置**，不在 feat-031 打轴优化范围内。

### 2. 打轴 residual（feat-031 → feat-033）

| 阶段 | 配置要点 | timing R | timing P | timing F1 | 说明 |
|---|---|---:|---:|---:|---|
| baseline-no-filter | patrol on, hyst=2, 丢空文本 | 83.9% | 100% | **91.2%** | 14 timing FN（8 no_overlap + 6 merged） |
| feat-033 final | patrol on, hyst=1, delay=2, 保留空文本 | 92.0% | 98.8% | **95.2%** | 门通过；1 FA；主剩余 text.noise / merged residual |

产物：`debug/benchmark-reports/baseline-no-filter/`、`debug/benchmark-reports/feat033_final/`。

feat-033 诊断：多数「空洞」是 OCR 抹段（M1c），不是 EMPTY 卡死。OCR 区域/水印仍后置。

## Benchmark 诊断口径

feat-027 将 benchmark 收敛为单一诊断口径：

| 维度 | 主指标 | 说明 |
|---|---|---|
| 打轴精准度 | `timing_recall` / `timing_precision` / `timing_f1` | 采用 GT 与 detection 一对一匹配，主依据为 `temporal_iou >= match_threshold`。 |
| 边界质量 | `start_mae_ms` / `end_mae_ms` / `boundary_p95_ms` | 只在一对一命中的条目上统计，用于判断字幕开始/结束偏移。 |
| 识别准确率 | `cer_macro` / `cer_micro` / `char_accuracy` / `exact_match_rate` | 只在时间命中的条目上统计；中文不再使用 WER 作为主指标。 |
| 端到端可用性 | `usable_subtitle_recall` | 默认要求时间命中且 `CER <= 20%`，衡量用户实际可用字幕比例。 |

诊断报告还会输出固定分类，供 agent 直接定位下一步工作：

- `timing.fn.no_overlap`
- `timing.fn.merged_into_neighbor`
- `timing.fn.boundary_miss`
- `timing.fp.false_alarm`
- `timing.fp.split_extra`
- `text.empty`
- `text.high_cer`
- `text.noise`

报告产物：

| 后缀 | 用途 |
|---|---|
| `.agent.json` | 结构化事实来源，含 run config、gates、summary、failure_clusters、cases、artifacts。 |
| `.gt_cases.csv` | 每条 GT 的命运：是否命中、边界误差、CER、分类与诊断 notes。 |
| `.det_cases.csv` | 每条 detection 的命运：是否匹配、是否 false alarm / split extra。 |
| `.summary.md` | 面向人的短报告，突出 conclusion、main_improvement、primary_remaining_gap。 |

## 复现方式

### Manifest 脚本

当前推荐把一次 benchmark 运行写成 manifest，再由脚本读取：

```bash
uv run python scripts/run_benchmark_manifest.py benchmark/manifests/zootopia_cli_bottomcrop_5fps.json
```

脚本输出当前诊断报告。入口是 `.agent.json`，后续 agent 应优先读取它，再按需打开
`.gt_cases.csv` / `.det_cases.csv` 定位具体条目。

GUI 选区路径只需要把 `region_box` 写入 manifest。坐标格式与 GUI / IPC 一致：
`[x, y, width, height]`，基于视频原始像素坐标，左上角为原点。

```json
{
  "video": "debug/Zootopia_clip_1080p.mp4",
  "ground_truth": "benchmark/fixtures/Zootopia_clip_1080p_gt.srt",
  "fps": 8.0,
  "engine": "vision",
  "confidence": 0.5,
  "match_threshold": 0.5,
  "region_box": [0, 842, 1920, 126],
  "label": "gui_region_8fps",
  "output_dir": "debug/benchmark-reports"
}
```

> `region_box` 必须与视频分辨率对应；若 GUI 在 1920×1080 视频上选择区域，
> benchmark 也应跑同一个 1920×1080 文件。换分辨率时需要按比例换算。

### Python API

```python
from pathlib import Path
from benchmark import RunConfig, run_benchmark, align_existing_srt
from benchmark.report import write_reports

# CLI 默认路径（会跑 pipeline，约 15s）
cli_cfg = RunConfig(
    video_path=Path("debug/Zootopia_clip_1080p.mp4"),
    ground_truth_path=Path("benchmark/fixtures/Zootopia_clip_1080p_gt.srt"),
    fps=5.0, engine="vision", label="cli_bottomcrop_5fps",
)
write_reports(run_benchmark(cli_cfg))

# 复现 GUI 历史产物（不跑 pipeline，对已有 SRT 对齐）
gui_cfg = RunConfig(
    video_path=Path("debug/Zootopia_clip_1080p.mp4"),
    ground_truth_path=Path("benchmark/fixtures/Zootopia_clip_1080p_gt.srt"),
    fps=8.0, engine="vision", label="gui_region_8fps_hist",
)
write_reports(align_existing_srt(gui_cfg, Path("debug/Zootopia_clip_1080p.srt")))

# 复现 GUI 配置但用指定 region_box 跑 pipeline（需知道坐标）
# cfg = RunConfig(..., region_box=(x, y, w, h), fps=8.0, label="gui_region_8fps")
# run_benchmark(cfg)
```

> **注**：GUI 历史产物的 `region_box` 坐标未记录，无法精确复现该次运行。后续 GUI 路径跑 benchmark 时务必把 `region_box` 写入 `RunConfig`，框架会记入报告。
