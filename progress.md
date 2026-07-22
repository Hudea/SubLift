# 会话进度日志

## 当前状态

- **最后更新：** 2026-07-17（Phase 4.1 立项）
- **当前 Phase：** phase4.1-post-roi-throughput
- **当前功能：** feat-042（已立项，尚未开始运行代码）
- **分支：** main
- **说明：** Phase 4 已合入本地 main；Phase 4.1 只规划 GUI 默认 path mode 的有界 producer/consumer 重叠，计划见 `docs/plans/phase4.1-post-roi-throughput.md`。

## 进行中

- [ ] **feat-042**：有界 path-mode 抽帧—Pipeline 重叠。先建立 job-local session、Queue(maxsize=8)、消费端进度和取消/重启隔离，再以固定 GT 与 clean-commit A/B 验收；不并行 OCR。

## 近期完成（最近 5 个）

- [x] **docs(phase4)**：新增 ROI 性能优化正式报告，归档 clean-commit A/B 结论与边界；Phase 4 已合入本地 main。
- [x] **feat-041**：性能计时归因收口；clean commit A/B 全部硬门/软目标通过，ROI coverage 三次均约 99.9996%。
- [x] **test(roi)**：P0/P1 验收测试（plan 路由矩阵、path 无 region、source-box 负向、compare_roi_ab 门脚本）；`pytest` 419 passed。
- [x] **feat-040**：≥10 分钟非 Zootopia 真实视频 path-mode 体验（首条/进度/取消/重启/SRT/RSS）。

## 阻塞项 / 风险

- [ ] **merged residual / #15 砰**：非 034 主目标，仍开放。
- [ ] **ground truth 素材有限**：性能 baseline 同 Zootopia 片源，不冒充泛化。
- [ ] **显式 CJK 混排风险**：边界 cleanup 可能误删无空格英文；默认 auto。
- [ ] **path-mode 并发风险**：producer/consumer 改造须避免旧 job 污染、取消/重启竞态与并发计时虚报；feat-042 尚未开始。

## 近期决策

- **ROI crop 在 RGB 空间**：`fps,format=rgb24,crop:exact=1`。
- **性能 coverage 采用排他编排 stage**：`pipeline_overhead` 扣除内层 leaf stages，避免双计。
- **Phase 4.1 不并行 OCR**：只让 extractor producer 与单消费者 Pipeline 重叠；并发 lane 不相加为 coverage。

> 完整决策记录见 `docs/DECISIONS.md`
