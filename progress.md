# 会话进度日志

## 当前状态

- **最后更新：** 2026-07-17（feat-039 完成，进入 feat-040）
- **当前 Phase：** phase4-roi-data-path
- **当前功能：** feat-040 非 Zootopia 长视频 GUI 真实验收
- **分支：** opt/roi-data-path
- **说明：** feat-038/039 已交付；下一步用 ≥10 分钟真实视频做 GUI path mode 体验验收。

## 进行中

- [ ] **feat-040**：≥10 分钟非 Zootopia 硬字幕 GUI 首条/进度/取消/重启/导出/RSS。

## 近期完成（最近 5 个）

- [x] **feat-039**：同提交 full/roi A/B 硬门全过（hash=b2d35c1e25f156e1，wall ratio 0.83）。
- [x] **fix(roi)**：crop 前 format=rgb24，对齐 full/roi 像素与 hash。
- [x] **feat-038**：固定区域 FFmpeg ROI 输出通路。
- [x] **feat-037**：开发者性能模式 + baseline。
- [x] **bugfix(GUI)**：多时间点扫描选区代表帧。

## 阻塞项 / 风险

- [ ] **merged residual / #15 砰**：非 034 主目标，仍开放。
- [ ] **ground truth 素材有限**：性能 baseline 同 Zootopia 片源，不冒充泛化。
- [ ] **feat-040**：需真实 GUI 证据（截图/录屏/带时间戳日志）；素材可用本地 Run On / 造雨人等。
- [ ] **显式 CJK 混排风险**：边界 cleanup 可能误删无空格英文；默认 auto。

## 近期决策

- **ROI crop 在 RGB 空间**：`fps,format=rgb24,crop:exact=1`，避免 yuv 色度漂移破坏 hash。
- **Phase 4 固定为 ROI 输出 + 真实长流验收**。
- **ROI 是输出优化，不是 ROI decode**。

> 完整决策记录见 `docs/DECISIONS.md`
