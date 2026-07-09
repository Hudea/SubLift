# 会话进度日志

## 当前状态

- **最后更新：** 2026-07-09
- **当前 Phase：** Phase 3 - 优化与基本可用
- **当前功能：** 统一抽帧 path mode（未提交）
- **分支：** opt/ocr-timeline
- **说明：** GUI 默认 `start_job.video_path` → Python `FfmpegExtractor`（ADR-0010）。feat-033 F1 95.2%@5fps / 96.4%@8fps。下一优先：GUI 实机复测对齐 live；merge residual / 更快默认档。

## 进行中

- 无（统一抽帧实现完成，待用户 GUI 验证 / 提交）

## 近期完成（最近 5 个）

- [x] 统一抽帧：path mode IPC + bridge + Swift 默认后端 ffmpeg；srt_loader 保留空 cue；ADR-0010。
- [x] feat-033：打轴 residual。F1 95.2%。详见 phase3.json。
- [x] feat-031：SSIM patrol。F1 +15.6pp，由 033 接力达门。
- [x] 实现 commit-based 自动 Label 与测试序号自动递增建档。
- [x] feat-027：Benchmark 框架完成。

## 阻塞项 / 风险

- [ ] **feat-028 跳过**：基线入库不做；对比用 baseline-no-filter / feat033_final 报告。
- [ ] **OCR 文本噪声后置**：text.noise / high_cer 仍是 e2e 主缺口（区域高度/英文水印）。
- [ ] **merged residual**：约 6 条 timing.fn.merged_into_neighbor 仍在；#15 砰仍 no_overlap。
- [ ] **precision 略降**：100%→98.8%（1 FA），未做更激进 CHANGE。
- [ ] **ground truth 素材有限**：主要依赖 Zootopia clip。

## 近期决策

- **ADR-0010**：打轴抽帧统一 Python `FfmpegExtractor`；GUI 默认 path mode，AVF 仅预览/选区。
- feat-033 默认：`hysteresis_frames=1`，`ocr_anchor_delay_frames=2`，`drop_empty_text=False`。
- 8fps 当前算法上限约 F1 96.4%（仍落后 goal 的 merge/起点）；默认速度档可仍 5fps。

> 完整决策记录见 `docs/DECISIONS.md`
