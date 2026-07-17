# 会话进度日志

## 当前状态

- **最后更新：** 2026-07-17（feat-038 完成，进入 feat-039）
- **当前 Phase：** phase4-roi-data-path
- **当前功能：** feat-039 ROI A/B 性能与固定 GT 回归
- **分支：** opt/roi-data-path
- **说明：** feat-038 已交付 crop-before-Python ROI 通路；下一目标是同提交 full/roi 硬门 A/B。

## 进行中

- [ ] **feat-039**：同提交 full vs roi（warmup=1, measured=3），硬门见 phase4 计划。

## 近期完成（最近 5 个）

- [x] **feat-038**：固定区域 FFmpeg ROI 输出通路（extractor/detector/pipeline/bridge/benchmark）。
- [x] **docs(benchmark)**：设计 `docs/design/benchmark.md` + 用法 `benchmark/README.md`。
- [x] **feat-037**：开发者性能模式 + 测量可信度补强 + baseline。
- [x] **bugfix(GUI)**：代表帧无字幕导致错选区 → 多时间点扫描 + 得分选帧。
- [x] **docs(GUI)**：macos-gui 统一到 path mode。

## 阻塞项 / 风险

- [ ] **merged residual / #15 砰**：非 034 主目标，仍开放。
- [ ] **ground truth 素材有限**：性能 baseline 同 Zootopia 片源，不冒充泛化。
- [ ] **Phase 4 长视频 GUI 体验验收**：需 ≥10 分钟非 Zootopia 真实视频，不能用 Mock 长流替代。
- [ ] **ROI 坐标风险**：旋转/显示坐标未验证时必须回退全帧；显式 ROI 越界必须报错，不能静默回退。
- [ ] **显式 CJK 混排风险**：边界 cleanup 可能误删无空格英文；默认 auto。

## 近期决策

- **Phase 4 固定为 ROI 输出 + 真实长流验收**：先做 fixed-region ffmpeg ROI，再做同提交 A/B，最后做真实 GUI；不混入质量泛化。
- **ROI 是输出优化，不是 ROI decode**：ffmpeg 仍可能完整解码；优化 stdout、PIL materialize 和重复 crop。
- **ROI 坐标双空间**：外部 region 为 source-frame，ROI Pipeline 只用 frame-local；无 region / 未验证旋转全帧回退。

> 完整决策记录见 `docs/DECISIONS.md`
