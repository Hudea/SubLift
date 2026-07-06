# 会话进度日志

## 当前状态

- **最后更新：** 2026-07-06
- **当前 Phase：** Phase 3 - 优化与基本可用
- **当前功能：** feat-027 Benchmark 框架（in-progress，待收尾）
- **分支：** main
- **说明：** Phase 3 规划已建立，目标限定为三项：benchmark 优化、增量处理与前台进度优化、打轴检测优化。OCR 区域裁剪、ASS/VTT、PaddleOCR、.app 打包等明确后置。

## 进行中

- [ ] feat-027：建立端到端 benchmark 框架（指标定义、ground truth 加载、一键运行、报告输出）。
  - 已新增 manifest 小入口：JSON 可记录 `region_box`，脚本可从 manifest 复现一次 benchmark 运行。

## 近期完成（最近 5 个）

- [x] feat-031：打轴检测优化（独立任务，patrol 推荐配置已落定）。F1 65%→80.6%（+15.6pp），recall 60.9%→90.8%，precision 不下降，merged_into_neighbor FN 减少约 70%。详见 phase3.json。
- [x] Phase 3 规划文档建立：`docs/plans/phase3.md`、`docs/phases/phase3.json`，更新 `feature-list.json`。
- [x] `apps/macos-gui` 分支合并到 `main`：Phase 2 所有计划内功能完成，feat-025 跳过。
- [x] feat-026：Phase 2 文档收尾全部完成。
- [x] feat-024：SRT 导出（Swift 端直接格式化 + NSSavePanel）。117 Swift 测试全绿。

## 阻塞项 / 风险

- [ ] **patrol 过切分**：7 组 GT 被切成两条检测段（OCR 噪声差异导致 dedupe 无法合并）。patrol 阈值 0.92 过敏感，后续可调到 0.88~0.90 或加 dedupe 模糊合并。详见 `docs/HURDLES.md`。
- [ ] **短字幕漏检**：`<=1200ms` 的 GT 只命中 7/16，主因是 hysteresis_frames=2 吃掉 40%+ 时长 + 单字字幕前景占比不足。后续可 hysteresis=1 + 降 presence_threshold，但需配合 patrol 阈值调优避免 FP。详见 `docs/HURDLES.md`。
- [ ] **Phase 3 范围控制**：用户明确先不规划具体实现方法，需在每个 feat 启动时重新评估技术方案，避免范围发散。
- [ ] **增量处理重构风险**：允许修改 pipeline/，可能引入回归；必须先完成 benchmark 基线再动手重构。
- [ ] **ground truth 素材有限**：目前主要依赖 Zootopia clip，benchmark 说服力可能不足。

## 近期决策

- feat-031 SSIM patrol 记为推荐配置（`enable_ssim_patrol=True, interval=3, threshold=0.92`），默认保持关闭。F1 +15.6pp 但未达 95% 目标。
- Phase 3 目标限定为：benchmark 优化、增量处理 + 前台进度优化、打轴检测优化。
- feat-031 属于项目打轴优化，不归入 benchmark 优化；规划顺序为 trace 归因 → SSIM patrol 补强连续字幕 CHANGE → 短字幕 IN/OUT 专项。

> 完整决策记录见 `docs/DECISIONS.md`
