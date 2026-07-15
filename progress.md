# 会话进度日志

## 当前状态

- **最后更新：** 2026-07-15（benchmark 文档重组）
- **当前 Phase：** phase3-opt-perf
- **当前功能：** 无进行中功能；下一项建议 ROI 输出
- **分支：** opt/perf
- **说明：** 删除 `docs/benchmarks/baseline.md`；设计 → `docs/design/benchmark.md`；用法/锚点 → `benchmark/README.md`。

## 进行中

- 无

## 近期完成（最近 5 个）

- [x] **docs(benchmark)**：设计 `docs/design/benchmark.md` + 用法 `benchmark/README.md`；移除散落 baseline.md。
- [x] **feat-037**：开发者性能模式 + 测量可信度补强 + baseline。
- [x] **bugfix(GUI)**：代表帧无字幕导致错选区 → 多时间点扫描 + 得分选帧。
- [x] **docs(GUI)**：macos-gui 统一到 path mode。
- [x] **feat-036 GUI**：左栏预览自适应布局。

## 阻塞项 / 风险

- [ ] **merged residual / #15 砰**：非 034 主目标，仍开放。
- [ ] **ground truth 素材有限**：性能 baseline 同 Zootopia 片源，不冒充泛化。
- [ ] **feat-030 长视频 GUI 手工验收暂缓**。
- [ ] **显式 CJK 混排风险**：边界 cleanup 可能误删无空格英文；默认 auto。

## 近期决策

- **Benchmark 文档分层**：ARCHITECTURE 索引 → design 设计 → `benchmark/README.md` 用法与锚点。
- **下一步性能优先 ROI 输出**：extract_wait + frame_materialize ≈ 39% core；不在 037 内实施。
- **性能优化先测量**：extract_wait 不得称为纯解码。

> 完整决策记录见 `docs/DECISIONS.md`
