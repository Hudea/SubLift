# 当前 ROI 通路性能归因基线

> **状态：** 当前默认固定 region ROI 路径的 clean-commit 性能锚
> **验证提交：** `1b4612b0a41c0804218e25cfd79d7a7724b2ee96`（`git_dirty=false`）
> **协议：** 同机、同负载、Apple Vision、warmup=1 + measured=3 独立进程，median 为主统计

## 目的与范围

本报告记录 Phase 4 ROI 数据通路完成、`pipeline_overhead` 补齐后的最终性能归因。它回答：
当前默认 ROI 路径在固定负载上的端到端成本在哪里，以及相对于 Full RGB 路径减少了什么。

它不是 codec 级 ROI decode 的声明，也不适合与其它机器、视频、codec、分辨率或 OCR 引擎的
数字直接相比较。`extract_wait` 包含解码、filter、RGB 转换和 stdout 输出等待，不能称为纯解码。

## 固定负载

| 项 | 固定值 |
|---|---|
| 视频 / GT | Zootopia 1920×1080、254.272 s；87 条固定 GT（视频不入库） |
| 采样与 OCR | 5fps、Apple Vision、`subtitle_script=cjk` |
| 区域 | source-frame `[0,848,1920,87]`，ROI 输出 1920×87 RGB |
| 对照 | Full RGB 输出 vs `fps,format=rgb24,crop:exact=1` ROI 输出 |
| 环境 | macOS arm64、Python 3.12.13、ffmpeg 8.1、Vision 可用 |
| 结果等价 | Full / ROI hash 均为 `b2d35c1e25f156e1`；ROI 三次质量门全通过 |

## Full 对 ROI 的最终结果

| 指标 | Full median | ROI median | ROI / Full | 结论 |
|---|---:|---:|---:|---|
| raw RGB 输出 | 7,906,636,800 B | 636,923,520 B | 8.06% | 减少 91.94% 无效 RGB 搬运 |
| `frame_materialize` | 1,912.2 ms | 233.2 ms | 12.20% | 显著降低 PIL 图像物化成本 |
| `core_wall` | 12,313.5 ms | 9,341.7 ms | 75.87% | ROI 实现 1.32× 相对吞吐 |
| Python peak RSS | 318.9 MB | 266.5 MB | 83.58% | 峰值减少约 16.4% |
| frame count | 1,271 | 1,271 | 100% | 帧时间序列等价 |
| Pipeline 二次 crop | — | 0（3/3） | — | ROI frame-local 坐标正确 |

ROI 三次 run 的 `stage_coverage_pct` 为 99.999778%、99.999784%、99.999730%，因此以下阶段
分布可作为同次 core wall 的可解释归因，而非重叠累计。

## ROI 阶段归因（3 次 measured 的 median）

| 阶段 | median total | core wall 占比 | 解释 |
|---|---:|---:|---|
| `ocr` | 4,825.5 ms | 51.65% | `OcrEngine.recognize()` 总 wall；当前最大单消费者成本 |
| `signature` | 1,493.0 ms | 15.98% | 字幕前景 / dHash 签名 |
| `extract_wait` | 1,404.0 ms | 15.03% | 解码、filter、RGB 与 pipe 等待的合成成本 |
| `changepoint` | 1,116.5 ms | 11.95% | 状态机与 CHANGE 判定 |
| `frame_materialize` | 233.2 ms | 2.50% | `Image.frombytes` |
| `color_convert` | 204.4 ms | 2.19% | RGB→BGR |
| `pipeline_overhead` | 73.8 ms | 0.79% | 排他编排成本；不会与叶子阶段双计 |

结论是：ROI 已把大图像物化从主要成本降为 2.5%，但没有加速单消费者 Vision OCR。因而
Phase 4.2 的第一步是分解 `ocr` 内部（输入准备、request、perform、observation mapping），
不是重新尝试 producer/consumer 并发或直接减少 OCR 调用数。

> 各阶段是分别对三次 measured run 取 median，不能横向相加为恰好等于 `core_wall`；每一次
> 原始 run 的 coverage 均约 100%，完整叶子项和对账口径见正式 Phase 4 报告。

## 同次质量回归

ROI 三次 measured run 均得到相同 hash，并全部通过：timing F1 97.7%、precision 98.8%、
usable 92.0%、CER macro 3.2%、text.noise 0、text.empty 0。性能结论不以质量回退交换。

## 复现与更新规则

```bash
uv run python scripts/run_benchmark_manifest.py \
  benchmark/manifests/zootopia_feat039_full.json --label manifest
uv run python scripts/run_benchmark_manifest.py \
  benchmark/manifests/zootopia_feat039_roi.json --label manifest
uv run python scripts/compare_roi_ab.py \
  <full.agent.json> <roi.agent.json> --out <ab_summary.json>
```

只有同一 clean commit、同机、相同负载、warmup=1 + measured=3，且 hash、质量、coverage 与
ROI A/B 硬门均通过时，才可更新本报告。完整 ROI 实现分析见
[Phase 4 性能报告](../../docs/reports/phase4-roi-performance.md)；后续 OCR 内部归因的验收见
[Phase 4.2 计划](../../docs/plans/phase4.2-ocr-performance-attribution.md)。
