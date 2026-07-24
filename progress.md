# 会话进度日志

## 当前状态

- **最后更新：** 2026-07-24（Phase 4.2 审查修复）
- **当前 Phase：** phase4.2-ocr-performance-attribution（进行中）
- **当前功能：** feat-043（in-progress：代码与自动对账硬门已补齐；真实 Vision 基线报告未完成）
- **分支：** main
- **说明：** Codex 质量审查后修复 P1 外层对账、P2 快速路径与 callback 隔离；状态从误标 completed 退回 in-progress，直至 canonical Vision 报告落地。

## 进行中

- [ ] **feat-043d 收口**：canonical Vision off/summary/trace 基线 + `docs/reports/phase4.2-ocr-attribution-baseline.md` + 硬门证据写入 phase4.2.json

## 近期完成（最近 5 个）

- [x] **feat-043 审查修复**：外层 call_count 三方对账、parent↔stages.ocr wall、Vision untimed 路径、callback 异常隔离；453 tests / ruff / mypy 通过。
- [x] **feat-043 初版实现**：有界归因模型、Vision observer、段内 trace、benchmark 内部对账（审查前误标完成）。
- [x] **docs(phase4)**：ROI 性能优化正式报告；Phase 4 合入本地 main。
- [x] **feat-041**：性能计时归因收口。
- [x] **feat-040**：≥10 分钟 path-mode 体验。

## 阻塞项 / 风险

- [ ] **merged residual / #15 砰**：非 034 主目标，仍开放。
- [ ] **ground truth 素材有限**：性能 baseline 同 Zootopia 片源，不冒充泛化。
- [ ] **显式 CJK 混排风险**：边界 cleanup 可能误删无空格英文；默认 auto。
- [ ] **feat-043 真实 Vision 基线**：完整 warmup+measured 约需多轮 4min+ 视频 OCR，本会话未跑。

## 近期决策

- **OCR 内部阶段不参与 core coverage 相加**：通过 `ocr_breakdown` 独立块输出，对账覆盖度 >= 80%。
- **Vision 内部计时通过可选 callback 接线**：无 callback 走 untimed 快速路径；callback 异常不污染 OCR 语义。
- **外层对账硬门**：`call_count` 必须与 `stages.ocr` / `throughput.ocr_calls` 一致；parent wall 与外层 ocr wall 交叉校验。

> 完整决策记录见 `docs/DECISIONS.md`
