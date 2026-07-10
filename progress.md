# 会话进度日志

## 当前状态

- **最后更新：** 2026-07-10
- **当前 Phase：** Phase 3 - 优化与基本可用
- **当前功能：** feat-034 完成；可选下一步 feat-029 增量 / merge residual / 文档 feat-032
- **分支：** opt/ocr-timeline
- **说明：** 行级选择器落地，usable 跨过 goal 水位。

## 进行中

- 无

## 近期完成（最近 5 个）

- [x] **feat-034** 全量：OcrLine + Profile + 行选 + 多帧共识 + cleanup。usable 89.7%。
- [x] path mode / ADR-0010 统一抽帧。
- [x] feat-033：打轴 residual。F1 95.2%。
- [x] feat-031：SSIM patrol。
- [x] feat-027：Benchmark 框架。

## 阻塞项 / 风险

- [ ] **feat-028 跳过**：基线对比用报告目录。
- [ ] **precision 略低**：97.6% vs 门 98.8%（2 FA，merge residual）。
- [ ] **merged residual / #15 砰**：非 034 主目标，仍开放。
- [ ] **ground truth 素材有限**：主要依赖 Zootopia clip。

## 近期决策

- **feat-034**：默认 `enable_line_select=True`；禁全局 conf 0.3；cleanup 去拉丁水印尾巴。
- **feat-034b**：profile 几何相对 crop。
- **ADR-0010**：打轴抽帧统一 `FfmpegExtractor`。

> 完整决策记录见 `docs/DECISIONS.md`
