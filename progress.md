# 会话进度日志

## 当前状态

- **最后更新：** 2026-07-17（feat-041 性能计时归因复验）
- **当前 Phase：** phase4-roi-data-path
- **当前功能：** feat-041 性能计时归因收口
- **分支：** opt/roi-data-path
- **说明：** ROI 通路、A/B 与长视频体验已完成；新增 `pipeline_overhead` 归因已实现并在未提交树复验，等待 clean-commit A/B 归档。

## 进行中

- [ ] **feat-041**：性能计时归因收口。代码、fake-clock 回归、真实 Vision A/B、`./init.sh` 与 `swift test` 已通过；因当前两组均为 dirty tree，待提交后以 clean commit 重跑 A/B，取得正式证据。

## 近期完成（最近 5 个）

- [x] **test(roi)**：P0/P1 验收测试（plan 路由矩阵、path 无 region、source-box 负向、compare_roi_ab 门脚本）；`pytest` 419 passed。
- [x] **feat-040**：≥10 分钟非 Zootopia 真实视频 path-mode 体验（首条/进度/取消/重启/SRT/RSS）。
- [x] **feat-039**：同提交 full/roi A/B 硬门全过（hash=b2d35c1e25f156e1）。
- [x] **fix(roi)**：crop 前 format=rgb24。
- [x] **feat-038**：固定区域 FFmpeg ROI 输出通路。

## 阻塞项 / 风险

- [ ] **merged residual / #15 砰**：非 034 主目标，仍开放。
- [ ] **ground truth 素材有限**：性能 baseline 同 Zootopia 片源，不冒充泛化。
- [ ] **显式 CJK 混排风险**：边界 cleanup 可能误删无空格英文；默认 auto。
- [ ] **feat-041 正式归档**：当前 A/B 的唯一失败为 `same_clean_commit`（两组均 dirty）；提交后需按同一 canonical 协议复跑，不以当前临时报告替代。

## 近期决策

- **ROI crop 在 RGB 空间**：`fps,format=rgb24,crop:exact=1`。
- **feat-040 证据形态**：path-mode 同源 Bridge + 时间戳 JSON 日志；视频不入库。
- **Phase 4 不混入质量泛化**。

> 完整决策记录见 `docs/DECISIONS.md`
