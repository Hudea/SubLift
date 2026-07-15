# Benchmark 设计

> `benchmark/` + `scripts/run_benchmark_manifest.py` + 可选 `src/sublift/diagnostics/performance.py`  
> 端到端质量诊断与开发者性能剖析。与产品提取管线同源，不另起算法实现。

用法与复现命令见 **[benchmark/README.md](../../benchmark/README.md)**。  
架构总览见 [ARCHITECTURE.md](../ARCHITECTURE.md) §11。

## 1. 设计原则

| 原则 | 说明 |
|---|---|
| **同源提取** | 使用产品 `Pipeline` + `FfmpegExtractor` + Detector + `OcrEngine`（ADR-0010），benchmark 不复制打轴/OCR 逻辑 |
| **诊断外置** | 匹配、指标、报告在 `benchmark/`；核心库只上报可选性能 span，不依赖 benchmark 包 |
| **单一质量口径** | feat-027 起统一一对一 temporal IoU + case 分类；不再维护旧宽松对齐 / WER 主指标 |
| **质量与性能同锚** | 性能结论必须附同次固定 GT 质量；禁止用牺牲 precision/usable 换速度 |
| **默认零扰动** | 性能模式默认 `off`；不创建 recorder，不改变提取语义；不暴露 GUI 用户开关 |

## 2. 模块职责

### 2.1 `benchmark/` 包

| 文件 | 职责 |
|---|---|
| `srt_loader.py` | 解析 SRT → `SrtEntry(index, start_ms, end_ms, text)`；GT 与检测共用 |
| `diagnostics.py` | 一对一匹配、GT/det case 分类、timing/recognition/e2e 指标、gates、failure_clusters、agent JSON / CSV / summary Markdown 格式化 |
| `runner.py` | `RunConfig` / `RunResult`、`run_benchmark`、`align_existing_srt`；warmup/measured、进程隔离、detection hash 与质量快照 |
| `manifest.py` | JSON manifest → `RunConfig`（路径相对仓库根解析） |
| `report.py` | `write_reports()` 落盘四件套（trace 时登记 segment JSONL artifact） |
| `git_utils.py` | `--label auto` 时按 commit 主题生成可递增输出子目录 |
| `manifests/*.json` | 可复现运行配方（视频、GT、fps、region、performance 等） |
| `fixtures/*.srt` | 入库 GT（视频本体在 `debug/`，不入版本控制） |

### 2.2 入口与辅助脚本

| 路径 | 职责 |
|---|---|
| `scripts/run_benchmark_manifest.py` | 一键：读 manifest → `run_benchmark` → `write_reports` |
| `scripts/measure_perf_overhead.py` | 同负载 3×off vs 3×summary，测 summary 中位扰动（开发者） |

### 2.3 产品路径上的可选埋点

| 路径 | 职责 |
|---|---|
| `src/sublift/diagnostics/performance.py` | `PerformanceMode` / `PerformanceRecorder`、叶子阶段归因、环境快照、多 run 聚合 |
| `src/sublift/extractor/ffmpeg_extractor.py` | 可选 recorder：`probe`、`extract_wait`、`frame_materialize`、raw bytes、frame count |
| `src/sublift/pipeline/core.py` | 可选 recorder：crop / color_convert / signature / changepoint / ocr / line_select / cleanup / consensus / finalize / dedupe；段级 trace |

CLI / GUI 默认不注入 recorder（`off` 语义：调用方不创建对象，热点路径一次 `is None`）。

## 3. 数据流

与 [ARCHITECTURE §4](../ARCHITECTURE.md) 产品数据流一致；benchmark 在其外包一层「配置 → 跑提 → 对齐诊断 → 报告」：

```text
manifest.json
  └─ load_run_config → RunConfig
       │
       ├─ [warmup_runs × 可选独立进程，丢弃结果]
       │
       └─ [measured_runs ×]
            video + RunConfig
              └─ FfmpegExtractor(fps[, performance_recorder])
                   └─ Pipeline(detector, ocr, config[, performance_recorder])
                        └─ list[SubtitleEntry]
                             └─ → list[SrtEntry]  detected
            GT SRT → list[SrtEntry]  ground_truth
              └─ analyze_result / 质量快照（detection_hash + gates）
                   └─ write_reports → .agent.json / .gt_cases.csv / .det_cases.csv / .summary.md
                        └─ [performance.mode≠off] 聚合 median + quality 跨 run 一致性
```

`align_existing_srt(config, detected_path)` 跳过提取，只对已有 SRT 做同一套诊断（复现 GUI/历史导出）。

### 3.1 与产品分层的关系

```text
┌─────────────────────────────────────────────┐
│  benchmark/  （诊断与编排，仓库根包）         │
│  manifest · runner · diagnostics · report   │
└───────────────────┬─────────────────────────┘
                    │ 调用
┌───────────────────▼─────────────────────────┐
│  src/sublift/  （产品核心，ADR 分层不变）     │
│  extractor · detector · ocr · pipeline      │
│  diagnostics/performance （可选、可空）       │
└─────────────────────────────────────────────┘
```

## 4. 运行配置

### 4.1 `RunConfig`（`runner.py`）

| 字段 | 默认 | 含义 |
|---|---|---|
| `video_path` / `ground_truth_path` | 必填 | 视频与 GT SRT |
| `fps` | 5.0 | 采样率，注入 `Config.sample_fps` 与 `FfmpegExtractor` |
| `engine` | `"vision"` | `vision` / `mock` |
| `confidence` | 0.5 | OCR 高置信门 |
| `subtitle_script` | `"auto"` | 无显式 profile 时的文字系统；固定中文 GT 常用 `"cjk"` |
| `match_threshold` | 0.5 | temporal IoU 一对一匹配阈值 |
| `region_box` | `None` | `[x,y,w,h]` → `FixedRegionDetector`；`None` → `BottomCropDetector` |
| `label` / `output_dir` | 可选 | 报告前缀与目录 |
| `video_duration_seconds` | 可选 | 覆盖 ffprobe 时长（速度指标） |
| `performance_mode` | `"off"` | `off` / `summary` / `trace` |
| `warmup_runs` | 0 | 丢弃的预热次数 |
| `measured_runs` | 1 | 计入聚合的次数 |
| `isolate_processes` | `True` | multi-run 时 spawn 独立进程隔离 peak RSS |

有 `region_box` 时 runner 用 `SubtitleProfile.from_crop(w, h, script=…)`，与 bridge 约定一致。

### 4.2 Manifest（`manifest.py`）

JSON object；相对路径相对**仓库根**（向上找 `pyproject.toml` + `feature-list.json`）。  
字段与 `RunConfig` 对齐；`performance` 为可选嵌套对象：

```json
"performance": {
  "mode": "off" | "summary" | "trace",
  "warmup_runs": 0,
  "measured_runs": 1
}
```

缺省 `performance` ≡ mode off、warmup 0、measured 1，旧 manifest 行为不变。

## 5. 质量诊断（feat-027）

实现集中在 `benchmark/diagnostics.py`。

### 5.1 一对一时间匹配

1. 对所有 det×gt 算 overlap 与 `temporal_iou`
2. 按 IoU 降序贪心匹配，且 `iou >= match_threshold`（默认 0.5）
3. 每个 GT / detection 最多匹配一条

### 5.2 主指标

| 维度 | 指标 | 口径 |
|---|---|---|
| 打轴 | `timing_recall` / `timing_precision` / `timing_f1` | 一对一命中数 / GT 数、/ det 数 |
| 边界 | `start_mae_ms` / `end_mae_ms` / `boundary_p95_ms` | 仅命中对 |
| 识别 | `cer_macro` / `cer_micro` / `char_accuracy` / `exact_match_rate` | 仅时间命中对；中文不以 WER 为主 |
| 可用性 | `usable_subtitle_recall` | 时间命中且 CER ≤ `DEFAULT_USABLE_CER_THRESHOLD`（0.20） |
| 速度 | `elapsed_seconds` / `speed_factor` | 视频时长 / wall（与 performance.core 可并存） |

### 5.3 Case 分类

| 类型 | 含义 |
|---|---|
| `ok` | 时间命中且文本可接受 |
| `timing.fn.no_overlap` | GT 无任何 det 重叠 |
| `timing.fn.merged_into_neighbor` | 相关 det 覆盖多条 GT |
| `timing.fn.boundary_miss` | 有重叠但 IoU 不足阈值 |
| `timing.fp.false_alarm` | det 未一对一匹配 |
| `timing.fp.split_extra` | 额外 det 叠在已匹配 GT 上 |
| `text.empty` / `text.high_cer` / `text.noise` | 命中后的文本问题 |

### 5.4 Gates（报告内）

| Gate | 目标 | blocking |
|---|---|---|
| `timing_f1` | ≥ `TIMING_F1_GATE`（0.95） | 是 |
| `timing_precision` | 不低于可选 baseline precision | 是（无 baseline 时 `pass=null`） |
| `usable_subtitle_recall` | ≥ `USABLE_RECALL_GATE`（0.80） | 否 |

固定 GT 回归验收（feat-033/034 产品门）更严，见 README 锚点表；runner 在 performance 多 run 时另用 feat-037 质量快照门（F1≥95.2%、P≥98.8%、usable≥85.1%、CER≤6.6%、noise≤2、empty≤1）。

### 5.5 报告产物

| 后缀 | 用途 |
|---|---|
| `.agent.json` | 结构化事实：run、gates、metrics、cases、clusters；可含 `performance` |
| `.gt_cases.csv` | 每条 GT 命运 |
| `.det_cases.csv` | 每条 detection 命运 |
| `.summary.md` | 人读结论 + 可选 Performance 总表 |
| `.perf-segments/*.jsonl` | 仅 `trace`：逐字幕段成本（**无原始字幕文本**） |

诊断阅读顺序：`.summary.md` 结论 → failure_clusters → `.gt_cases.csv` / `.det_cases.csv` 定位 → 必要时 trace / pipeline 诊断脚本。

## 6. 性能诊断（feat-037）

实现：`src/sublift/diagnostics/performance.py` + `runner` 编排。

### 6.1 模式

| 模式 | 行为 |
|---|---|
| `off` | 不创建 recorder；报告无 `performance`（单 run 默认） |
| `summary` | 有界阶段聚合；OCR 固定上限样本求 p50/p95 |
| `trace` | summary + 逐段 JSONL |

### 6.2 阶段口径（叶子阶段计入 coverage）

| Stage | 含义 |
|---|---|
| `probe` | ffprobe 尺寸探测 |
| `extract_wait` | stdout 读满一帧等待：**decode + filter + RGB 转换 + 管道输出**（**不是**纯 codec decode） |
| `frame_materialize` | `Image.frombytes` 构造 PIL |
| `crop` / `color_convert` | 区域裁剪、RGB→BGR |
| `signature` / `changepoint` | 打轴 |
| `ocr` | `OcrEngine.recognize` wall |
| `line_select` / `cleanup` / `consensus` | 行级选择与共识（与 Vision 分开） |
| `dedupe` | 最终合并 |
| `finalize` | **容器 span**，coverage **排除**，避免与内部 ocr/dedupe 双重计数 |

吞吐字段：`core_wall_ms`、`realtime_factor`、`raw_output_bytes`、`frame_count`、`ocr_calls`、`attributed_stage_ms`、`unattributed_ms`、`stage_coverage_pct`。  
延迟：`first_frame_ms`、`first_entry_ms`、`spawn_to_first_frame_ms`（相对 core 起点；无字幕时 first_entry 可为 null）。  
资源：Python peak RSS（macOS `ru_maxrss` 为 bytes）、user/system CPU 秒。

### 6.3 多 run 与质量一致性

- `warmup_runs` 完整跑但丢弃；`measured_runs` 进入 median/min/max
- multi-run 默认 `spawn` 独立进程；**先** `queue.get(timeout)` **再** `join`，避免大结果管道死锁
- 每次 measured 计算 `detection_hash`（start/end/text）与质量 checks；报告 `detections_consistent` / `all_runs_pass`

### 6.4 环境元数据

`collect_environment()`：git commit/dirty、platform/machine、Python、ffmpeg 版本行、Vision 是否可用等。跨机器结果只可参考，直接对比需同环境同负载。

## 7. 固定负载与已知水位

Canonical 素材（视频不入库）：

| 项 | 值 |
|---|---|
| 视频 | `debug/Zootopia_clip_1080p.mp4`（1080p，约 254.27s） |
| GT | `benchmark/fixtures/Zootopia_clip_1080p_gt.srt`（87 条） |

**质量回归锚点**（feat-034 P1 fix2，统一 diagnostic 口径，固定 region + cjk）：  
timing F1 **97.7%**、precision **98.8%**、usable **92.0%**、CER macro **3.2%**、noise/empty **0**。  
详见 [benchmark/README.md](../../benchmark/README.md) 锚点表；历史阶段数字见 phase evidence / `debug/benchmark-reports/`。

**性能协议**（feat-037）：同素材、5fps、Vision、`region [0,848,1920,87]`、`subtitle_script=cjk`、warmup=1 + measured=3、median 主统计；manifest  
`benchmark/manifests/zootopia_fixed_region_5fps_perf.json`，默认输出 `debug/perf_reports/`。  
阶段占比以实跑 `.agent.json` 为准；典型量级 OCR 与「extract_wait + frame_materialize」同属第一梯队（约各 40% 量级），支撑后续 ROI 输出优化立项，**本模块不实现 ROI**。

## 8. 约束与非目标

- 不替代 CLI/GUI 产品入口；不把 profiler 默认打开
- 不将 `extract_wait` 报告为「纯解码」
- 不在 GUI 增加性能模式用户开关
- 单一 Zootopia GT **不**代表跨片源泛化
- ASS/VTT、PaddleOCR、多引擎对照不在本模块范围

## 9. 相关文档

- 用法：[benchmark/README.md](../../benchmark/README.md)
- 架构：[ARCHITECTURE.md](../ARCHITECTURE.md)
- 管线 / OCR / 抽帧：[pipeline.md](pipeline.md) · [ocr.md](ocr.md) · [extractor.md](extractor.md)
- 计划：`docs/plans/phase3-opt-perf.md`
