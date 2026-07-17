# Phase 4 ROI 数据通路 — 执行进度

## Phase 名称与目标

- **Phase**：`phase4-roi-data-path`
- **分支**：`opt/roi-data-path`
- **目标**：固定字幕区域 ffmpeg crop-before-Python；同提交 full/roi A/B；≥10 分钟非 Zootopia GUI 长流验收。

## Feature 列表与依赖

| ID | 名称 | 依赖 | 状态 | Commit |
|---|---|---|---|---|
| feat-038 | 固定区域 FFmpeg ROI 输出通路 | feat-037 | **done** | `4b5eea3` |
| feat-039 | ROI A/B 性能与固定 GT 回归 | feat-038 | **done** | `a6a1535` |
| feat-040 | 非 Zootopia 长视频 GUI 真实验收 | feat-039 | **done** | `830266f` |
| feat-041 | 性能计时归因收口 | feat-039 | **done** | `1b4612b` |

## 当前执行状态

- **当前 Feature**：无（Phase 4 features 完成）
- **当前步骤**：Phase 级验收

## feat-039 / feat-041 关键结果（clean 1b4612b）

| 指标 | full | roi | 门 |
|---|---:|---:|---|
| detection_hash | b2d35c1e25f156e1 | b2d35c1e25f156e1 | 相等 |
| raw_output_bytes | 7,906,636,800 | 636,923,520 | 87/1080 |
| frame_materialize median ms | 1912 | 233 | ratio 0.122 ≤ 0.25 |
| core_wall median ms | 12314 | 9342 | ratio 0.759 ≤ 1.05 |
| peak RSS ratio | — | 0.836 | ≤ 1.05 |
| 固定 GT | — | 三次全过 | 是 |

软目标：wall ≤90% full；realtime_factor ≈1.32× full。

## 审查

| Feature | 轮次 | 结论 |
|---|---|---|
| feat-038 | 1 | Changes Required → 已修 |
| feat-038 | 2 | Approved with Minor Notes |
| feat-039 | 1 | Approved with Minor Notes（compare 质量门加固已做） |

## feat-040 摘要

- 素材 01.mp4（造雨人，非 Zootopia）2533s；region [0,880,1920,180]
- 首条 1.60s；cancel latency 2.9ms；restart OK；SRT 987 条再打开 OK
- RSS 峰值 ~305MB；证据 debug/feat040_ux/

## 下一步

Phase 级验收与正式性能报告均已收口；见 `docs/reports/phase4-roi-performance.md`。
