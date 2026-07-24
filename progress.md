# 会话进度日志

## 当前状态

- **最后更新：** 2026-07-24（Phase 4.2 立项）
- **当前 Phase：** phase4.2-ocr-performance-attribution
- **当前功能：** feat-043（已立项，尚未开始运行代码）
- **分支：** main
- **说明：** Phase 4 已合入本地 main。Phase 4.1 的有界 producer/consumer 实验未稳定通过真实 Vision 吞吐门，未采纳；当前保持串行 path mode。Phase 4.2 先建立 OCR 内部归因基线，计划见 `docs/plans/phase4.2-ocr-performance-attribution.md`。

## 进行中

- [ ] **feat-043**：Vision OCR 内部归因与决策基线。先完成不双计、有界且无文本泄露的 summary/trace，再用 canonical Vision 的 off/summary/trace 对照决定下一项优化；不改 OCR/打轴算法。

## 近期完成（最近 5 个）

- [x] **docs(phase4)**：新增 ROI 性能优化正式报告，归档 clean-commit A/B 结论与边界；Phase 4 已合入本地 main。
- [x] **feat-041**：性能计时归因收口；clean commit A/B 全部硬门/软目标通过，ROI coverage 三次均约 99.9996%。
- [x] **feat-040**：≥10 分钟非 Zootopia 真实视频 path-mode 体验（首条/进度/取消/重启/SRT/RSS）。
- [x] **feat-042（归档）**：producer/consumer 机制、质量、取消均通过，但真实 Vision 两轮 A/B 未过 wall≤串行95% 门；实验未入 main，证据见 `docs/phases/phase4.1.json`。
- [x] **docs(benchmark)**：将固定 GT 质量锚与最终 ROI 性能归因整理为 `benchmark/reports/` 的版本化快照；原始 debug 产物继续忽略。

## 阻塞项 / 风险

- [ ] **merged residual / #15 砰**：非 034 主目标，仍开放。
- [ ] **ground truth 素材有限**：性能 baseline 同 Zootopia 片源，不冒充泛化。
- [ ] **显式 CJK 混排风险**：边界 cleanup 可能误删无空格英文；默认 auto。
- [ ] **OCR 归因尚粗**：现有 `ocr` 仅是总 wall，尚不能区分 Vision perform、PIL/CGImage 桥接和结果映射；feat-043 未开始。

## 近期决策

- **ROI crop 在 RGB 空间**：`fps,format=rgb24,crop:exact=1`。
- **性能 coverage 采用排他编排 stage**：`pipeline_overhead` 扣除内层 leaf stages，避免双计。
- **Phase 4.1 不采纳**：真实 Vision overlap 结果等价却未稳定达到 wall≤串行95%，保留串行默认并转向 OCR 内部归因。

> 完整决策记录见 `docs/DECISIONS.md`
