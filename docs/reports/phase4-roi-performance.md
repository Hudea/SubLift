# Phase 4 ROI 数据通路性能优化报告

> **状态**：已验收<br>
> **日期**：2026-07-17<br>
> **验证提交**：`1b4612b0a41c0804218e25cfd79d7a7724b2ee96`（clean tree）<br>
> **正式证据**：`debug/benchmark-reports/phase4-history-rewrite-ab/ab_summary.json`

## 1. 结论

对已知、有效的固定字幕区域，SubLift 已将默认 GUI path mode 的数据通路从
「ffmpeg 输出完整画面，Python 再裁字幕带」改为「ffmpeg 在 RGB stdout 前裁出字幕
ROI，Python 只消费局部图像」。

在 canonical 1080p / 5fps / Vision 负载上，该优化在**不改变检测结果和固定 GT 质量**的
前提下，验证了以下结论：

- raw RGB 输出从 **7.91 GB** 降至 **636.9 MB**，即全帧路径的 **8.06%**
  （减少 **91.94%**）；
- `frame_materialize` 中位耗时从 **1912.2 ms** 降至 **233.2 ms**，即全帧路径的
  **12.20%**（减少 **87.80%**）；
- Python peak RSS 从 **318.9 MB** 降至 **266.5 MB**（减少 **16.42%**）；
- core wall 中位耗时从 **12,313.5 ms** 降至 **9,341.7 ms**，即 ROI 为 Full 的
  **75.87%**、相对吞吐约 **1.32×**；
- ROI 与 Full 的 detection hash 均为 `b2d35c1e25f156e1`，三次 measured run 的质量
  指标完全一致。

这是一次**数据传输、图像物化和 Python 侧内存路径**的优化；它不是 codec 级 ROI decode，
也不承诺任何视频、codec 或字幕区域上的同等端到端加速。

## 2. 问题与目标

优化前，即使 GUI 已知用户选中的固定字幕区域，`FfmpegExtractor` 仍向 Python 输出完整
1920×1080 RGB 帧；随后 Pipeline 才对 `[0, 848, 1920, 87]` 裁剪。对于 5fps、1271 帧的
canonical 素材，这意味着大量不参与 OCR、签名或打轴的像素仍经历：

```text
视频解码 → 完整 RGB stdout → Python pipe read → PIL Image.frombytes
      → Pipeline crop → signature / OCR
```

本阶段目标是消除其中的**无效 RGB 输出、传输、PIL 物化与重复 crop**，同时证明坐标语义、
打轴和 OCR 结果均不回退。

## 3. 实现方案

### 3.1 路径变化

```text
Full（对照）
视频 → ffmpeg fps,format=rgb24 → 完整 RGB → Python → FixedRegion crop

ROI（实验）
视频 → ffmpeg fps,format=rgb24,crop:exact=1 → ROI RGB → Python
     → RoiPassthroughDetector([0,0,w,h]) → 零二次 crop
```

关键约束：

- source-frame 选区只供 ffmpeg crop 和诊断使用；进入 Pipeline 后，ROI 图像统一使用
  frame-local 坐标；
- `RoiPassthroughDetector` 返回局部全幅 `Region([0, 0, w, h])`；
- Pipeline 在 Region 等于输入图全幅时直接透传，`pipeline_crop_count` 必须为 0；
- 无固定 region、legacy frame mode、BottomCrop 或未经验证的旋转映射，继续走全帧安全
  回退路径；
- filter 固定为 `fps,format=rgb24,crop=<w>:<h>:<x>:<y>:exact=1`，保证 ROI 像素与 Full
  路径在 Python 裁剪后的像素一致。

详细坐标与路由契约见 [ROI 数据通路设计](../design/roi-data-path.md)。

### 3.2 端到端优化路径

固定 region 的产品与 benchmark 都经过同一条 Python 数据路径：

```text
Swift GUI 选区 / benchmark manifest（source-frame region）
  → BridgeHandler / RunConfig
  → plan_frame_io：一次决定 full 或 roi、extractor 与 detector
  → FfmpegExtractor(output_crop=region)
  → ffmpeg: fps,format=rgb24,crop:exact=1
  → ROI Frame（frame-local）
  → RoiPassthroughDetector([0,0,w,h])
  → Pipeline：全幅透传、signature、changepoint、OCR、export
  → PerformanceRecorder / compare_roi_ab：质量与性能联合验收
```

这里的关键不是单独替换一个 crop 调用，而是同一次 `plan_frame_io` 同时确定 extractor
输出坐标和 detector 消费坐标。这样 source-frame box 不会被错误地再次应用到 ROI 图像上，
也不会因多处路由判断而出现 Full / ROI 语义漂移。

### 3.3 优化目标、代码落点、达成结果与证据

| 优化路径 / 目标 | 代码落点 | 达成结果 | 自动测试与实测证据 |
|---|---|---|---|
| 只在有效固定 region 时启用 ROI；其他路径安全回退 | [frame_io.py](../../src/sublift/extractor/frame_io.py) 的 `plan_frame_io`；[bridge.py](../../src/sublift/ipc/bridge.py) path mode；[runner.py](../../benchmark/runner.py) A/B 路由 | GUI path mode 默认受益；无 region 仍为 Full + BottomCrop，旋转未验证时不冒险裁剪 | [ROI 路由测试](../../tests/test_roi_output_path.py) 覆盖 `test_plan_auto_with_region_uses_roi`、`test_plan_no_region_uses_bottom_crop`、`test_path_mode_without_region_full_bottom_crop`；正式 A/B 的 `output_mode` 检查通过 |
| 在 Python 前减少 RGB 输出与 pipe 传输 | [ffmpeg_extractor.py](../../src/sublift/extractor/ffmpeg_extractor.py) 的 `output_crop`、校验与 raw RGB 读取；[frame_io.py](../../src/sublift/extractor/frame_io.py) 的 `build_output_vf` | 输出从 7,906,636,800 B 降至 636,923,520 B，严格为 Full 的 8.06% | `test_roi_frame_size_and_bytes`、`test_roi_pixels_match_full_then_crop`、`test_roi_frame_count_and_timestamps_match_full`；clean A/B `raw_output_bytes_ratio` 通过 |
| 消除 source / local 坐标混用与二次 PIL crop | [roi_passthrough.py](../../src/sublift/detector/roi_passthrough.py)；[core.py](../../src/sublift/pipeline/core.py) 的 `_crop_to_region` | ROI Region 恒为局部全幅，Pipeline 不再复制同尺寸图像；三次 measured `pipeline_crop_count=0` | `test_fixed_region_source_box_on_roi_image_not_passthrough`、`test_roi_passthrough_preserves_pixels`、`test_roi_path_pipeline_crop_count_zero`；Full/ROI hash 一致 |
| 保持固定 GT 质量，而非以速度换质量 | [runner.py](../../benchmark/runner.py) 多进程 measured run；[compare_roi_ab.py](../../scripts/compare_roi_ab.py) | 三次 ROI 都得到相同 hash，F1 97.7%、precision 98.8%、usable 92.0%、CER 3.2%、noise/empty 0 | [A/B 硬门测试](../../tests/test_compare_roi_ab.py) 覆盖 hash、raw bytes、quality、crop 和 clean-commit 失败条件；正式 `compare_roi_ab` 全部硬门通过 |
| 让 core wall 可解释，避免把未归因时间错归 ROI 或 Vision | [performance.py](../../src/sublift/diagnostics/performance.py) 的 `exclusive_span`；[core.py](../../src/sublift/pipeline/core.py) 的 `pipeline_overhead` | ROI 三轮 coverage 为 99.999778% / 99.999784% / 99.999730%，unattributed 仅为 0.021 / 0.020 / 0.025 ms 的计时精度级残差 | [性能记录器测试](../../tests/test_performance_recorder.py) 的 `test_exclusive_span_records_only_uncovered_pipeline_time`；正式 A/B `stage_coverage` 通过 |

该映射说明：每项结果都有对应的实现位置和至少一类自动或真实负载证据；不把单次 wall
变化孤立地当成优化结论。

### 3.4 性能归因修正

首次 A/B 发现少量 core wall 未归入已有 stage。为避免把这部分时间误解为 ROI 或 Vision
成本，新增了排他 `pipeline_overhead`：

```text
pipeline_overhead
  = Pipeline.run_frames 外层 wall
  - 其中互不重叠的 leaf stages（OCR、crop、signature 等）
```

它归属帧迭代、状态机/Timeline 分派、容器自身和 recorder 固定开销；`finalize` 仍为容器
stage，内部 `ocr` / `dedupe` 不会被重复计入。fake-clock 回归测试证明 leaf stages 与
`pipeline_overhead` 可完整覆盖 core wall 且无双计。

## 4. 验证协议

| 项目 | 固定值 |
|---|---|
| 素材 | `debug/Zootopia_clip_1080p.mp4`，1920×1080，约 254.3 秒 |
| GT | `benchmark/fixtures/Zootopia_clip_1080p_gt.srt`，87 条 |
| 选区 | source-frame `[0, 848, 1920, 87]` |
| 采样与 OCR | 5fps、Apple Vision、`subtitle_script=cjk` |
| 对照 | `frame_output_mode=full` 对 `frame_output_mode=roi` |
| 运行方式 | warmup=1，measured=3；每次 measured 为独立进程 |
| 环境 | macOS arm64、Python 3.12.13、ffmpeg 8.1、Vision 可用 |
| 可比性 | 两组均来自同一 clean commit `1b4612b`，`git_dirty=false` |

所有数字均取三次 measured run 的中位数，除非表中另有说明。原始 agent JSON、CSV 与
summary 位于 `debug/benchmark-reports/phase4-history-rewrite-ab/`（本地归档，不入版本库）。

## 5. 验收结果

### 5.1 正确性与质量

| 验收项 | 结果 | 状态 |
|---|---:|:---:|
| frame count | Full = ROI = 1271 | 通过 |
| detection hash | Full = ROI = `b2d35c1e25f156e1` | 通过 |
| timing F1 | 97.7%（ROI 三次一致） | 通过 |
| timing precision | 98.8%（ROI 三次一致） | 通过 |
| usable subtitle recall | 92.0%（ROI 三次一致） | 通过 |
| CER macro | 3.2%（ROI 三次一致） | 通过 |
| text noise / empty | 0 / 0（ROI 三次一致） | 通过 |
| Pipeline 二次 crop | ROI 三次均为 0 | 通过 |

因此，本优化证明的是**同一输入像素语义下的等价结果**，并非以牺牲精度或可用性换取速度。

### 5.2 性能与资源

| 指标 | Full | ROI | ROI / Full | 结论 |
|---|---:|---:|---:|---|
| raw output bytes | 7,906,636,800 B | 636,923,520 B | 8.06% | 减少 91.94% |
| raw bytes / frame | 6,220,800 B | 501,120 B | 8.06% | 与 87 / 1080 面积比一致 |
| `frame_materialize` | 1,912.2 ms | 233.2 ms | 12.20% | 减少 87.80% |
| core wall | 12,313.5 ms | 9,341.7 ms | 75.87% | 约 1.32× 加速 |
| realtime factor | 20.65× | 27.22× | 131.81% | 提升 31.81% |
| Python peak RSS | 318.9 MB | 266.5 MB | 83.58% | 减少约 52.4 MB |
| first frame | 114.7 ms | 60.4 ms | 52.62% | 降低约 47.4% |
| first entry | 1,102.8 ms | 848.3 ms | 76.93% | 降低约 23.1% |

性能硬门全部通过：

- ROI `raw_bytes_per_frame = 501,120`；
- raw output 比例严格为 `87 / 1080`；
- `frame_materialize ≤ 25%` Full（实际 12.51%）；
- core wall 与 peak RSS 均不超过 Full 的 105%；
- 三次 ROI `stage_coverage_pct ≥ 99%`（实际 99.999778% / 99.999784% /
  99.999730%）。

两个非阻断软目标也同时达成：core wall ≤ Full 的 90%（实际 83.85%），realtime factor
≥ Full 的 1.10×（实际 1.32×）。

## 6. 如何理解收益

ROI 的 raw RGB 路径缩小约 12.4 倍，并不意味着端到端 wall 也应缩小 12.4 倍。原因是：

1. 常见 H.264 / HEVC 编码帧通常仍需完整重建，ffmpeg crop 发生在解码和 RGB 转换之后；
2. Apple Vision OCR、帧签名、变化点检测仍会运行，且 Vision 调用存在进程、缓存和系统
   调度方差；
3. peak RSS 包含 Python、OpenCV、Vision/Objective-C 与分配器保留的固定工作集，不等于
   单帧 RGB 缓冲的大小；
4. `extract_wait` 的定义是 decode + filter + RGB + stdout pipe，并非纯 codec decode。

因此，对外和后续开发应把这一结果表述为：**固定字幕区域下，ROI 路径稳定降低数据搬运、
图像物化和 Python 内存压力，并在 canonical 负载上实测带来约 19% 端到端吞吐提升。**

不应表述为“只解码字幕区域”或“所有视频均可 12 倍加速”。

## 7. 范围与剩余边界

- 本报告只覆盖固定 region 的 GUI path mode 与 benchmark ROI 路径；
- 不覆盖 CLI BottomCrop 自动 ROI、动态区域、缩放、JPEG 重编码、旋转坐标映射或并发
  pipeline；
- 固定 GT 主要为 Zootopia 中文字幕片段，不能据此宣称英文、中英混排、不同字幕位置或
  不同片源的质量泛化；
- 当前质量 residual 仍包括少量 `text.high_cer`、merged segment 与短字幕 timing case，
  它们属于后续质量泛化/打轴任务，不由 ROI 优化解决。

## 8. 复现与审计

在同一台机器、clean working tree 中使用以下标准 manifest 复跑：

```bash
uv run --extra vision python scripts/run_benchmark_manifest.py \
  benchmark/manifests/zootopia_feat039_full.json --label <full-label>

uv run --extra vision python scripts/run_benchmark_manifest.py \
  benchmark/manifests/zootopia_feat039_roi.json --label <roi-label>

uv run python scripts/compare_roi_ab.py \
  <full-agent.json> <roi-agent.json> --out <ab-summary.json>
```

比较器必须同时验证 clean commit、hash、质量门、raw bytes、materialize、wall、RSS、
coverage 与 `pipeline_crop_count`。若任一项失败，不能用单项性能数字替代整体验收结论。

## 9. 相关文档

- [Phase 4 计划](../plans/phase4-roi-data-path.md)
- [ROI 数据通路设计](../design/roi-data-path.md)
- [Benchmark 设计](../design/benchmark.md)
- [ADR-0014 / ADR-0015](../DECISIONS.md)
