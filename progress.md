# 会话进度日志

## 当前状态

- **最后更新：** 2026-07-24（Phase 4.2 已收口）
- **当前 Phase：** phase4.2-ocr-performance-attribution（已完成）
- **当前功能：** 下一项尚未立项；方向为多源 GT 后的代表帧排序与有效 OCR 调用实验
- **分支：** main
- **说明：** canonical off/summary/trace 已跑完；`vision_perform≈99%` OCR parent；正式报告见 `docs/reports/phase4.2-ocr-attribution-baseline.md`。summary 的 1.319 扰动限制其作为产品速度基线，不影响归因分流结论。

## 进行中

- 暂无。下一次开工先规划并补齐英文、中英混排、不同位置与不同片源的固定 GT。

## 近期完成（最近 5 个）

- [x] **feat-043 Vision OCR 内部归因**：完成真实 Vision 基线与分流；默认路径不变，下一方向为多源 GT 后的代表帧排序/有效调用实验。
- [x] **feat-043 审查修复与历史整理**：外层对账、untimed 路径；phase4.2 commits 压成 `abafab2`。
- [x] **feat-043 初版实现**：有界归因 / observer / 段 trace / benchmark 对账。
- [x] **docs(phase4)**：ROI 性能优化正式报告。
- [x] **feat-041**：性能计时归因收口。

## 阻塞项 / 风险

- [ ] **merged residual / #15 砰**：非 034 主目标，仍开放。
- [ ] **ground truth 素材有限**：性能 baseline 同 Zootopia 片源，不冒充泛化。
- [ ] **显式 CJK 混排风险**：边界 cleanup 可能误删无空格英文；默认 auto。
- [ ] **summary 不能作产品速度基线**：1.319 > 1.05；根因未被本轮分块三次测量证明。仅在未来需要以 summary 承诺速度时，做交错 off/summary 配对复测。

## 近期决策

- **Vision 请求执行 ≈99% OCR parent**：不做桥接、映射、并行或默认几何微优化；先补多源 GT，再研究代表帧排序与有效调用数。
- **summary 读数用途受限**：扰动未过不影响归因收口，但不得用作产品速度数字。
- **无 callback 走 untimed 快速路径**：observer 异常不污染 OCR 语义。

> 完整决策记录见 `docs/DECISIONS.md`
