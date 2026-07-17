# 会话进度日志

## 当前状态

- **最后更新：** 2026-07-17（Phase 4 收口）
- **当前 Phase：** phase4-roi-data-path
- **当前功能：** 无进行中；Phase 4 features 均 done
- **分支：** opt/roi-data-path
- **说明：** ROI 通路 + A/B 硬门 + 长视频 path-mode 体验均完成。

## 进行中

- 无

## 近期完成（最近 5 个）

- [x] **feat-040**：≥10 分钟非 Zootopia 真实视频 path-mode 体验（首条/进度/取消/重启/SRT/RSS）。
- [x] **feat-039**：同提交 full/roi A/B 硬门全过（hash=b2d35c1e25f156e1）。
- [x] **fix(roi)**：crop 前 format=rgb24。
- [x] **feat-038**：固定区域 FFmpeg ROI 输出通路。
- [x] **feat-037**：开发者性能模式 + baseline。

## 阻塞项 / 风险

- [ ] **merged residual / #15 砰**：非 034 主目标，仍开放。
- [ ] **ground truth 素材有限**：性能 baseline 同 Zootopia 片源，不冒充泛化。
- [ ] **显式 CJK 混排风险**：边界 cleanup 可能误删无空格英文；默认 auto。

## 近期决策

- **ROI crop 在 RGB 空间**：`fps,format=rgb24,crop:exact=1`。
- **feat-040 证据形态**：path-mode 同源 Bridge + 时间戳 JSON 日志；视频不入库。
- **Phase 4 不混入质量泛化**。

> 完整决策记录见 `docs/DECISIONS.md`
