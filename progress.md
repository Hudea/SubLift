# 会话进度日志

## 当前状态

- **最后更新：** 2026-07-09
- **当前 Phase：** Phase 3 - 优化与基本可用
- **当前功能：** 无（feat-033 打轴 residual 已完成）
- **分支：** opt/ocr-timeline
- **说明：** timing_f1 基线 91.2% → **95.2%**（门通过）。下一优先：OCR text.noise 区域/水印（后置），或 feat-029 增量处理。

## 进行中

- 无

## 近期完成（最近 5 个）

- [x] feat-033：打轴 residual。诊断 M1c；锚帧延迟+空文本保留+hysteresis=1。F1 95.2%。详见 phase3.json。
- [x] feat-031：SSIM patrol。F1 +15.6pp，未达 95% 门（由 033 接力）。
- [x] 实现 commit-based 自动 Label 与测试序号自动递增建档。
- [x] benchmark 产物目录调整：默认输出改为 `debug/benchmark-reports`。
- [x] feat-027：Benchmark 框架完成。

## 阻塞项 / 风险

- [ ] **feat-028 跳过**：基线入库不做；对比用 baseline-no-filter / feat033_final 报告。
- [ ] **OCR 文本噪声后置**：text.noise / high_cer 仍是 e2e 主缺口（区域高度/英文水印）。
- [ ] **merged residual**：约 6 条 timing.fn.merged_into_neighbor 仍在；#15 砰仍 no_overlap。
- [ ] **precision 略降**：100%→98.8%（1 FA），未做更激进 CHANGE。
- [ ] **ground truth 素材有限**：主要依赖 Zootopia clip。

## 近期决策

- feat-033 默认：`hysteresis_frames=1`，`ocr_anchor_delay_frames=2`，`drop_empty_text=False`。
- 空洞主因是 OCR 抹段（M1c），不是状态机未重新 IN。
- timing_f1≥95% 门由 feat-033 关闭；OCR 区域优化仍后置。

> 完整决策记录见 `docs/DECISIONS.md`
