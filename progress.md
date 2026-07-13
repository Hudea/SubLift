# 会话进度日志

## 当前状态

- **最后更新：** 2026-07-13
- **当前 Phase：** Phase 3 - 优化与基本可用
- **当前功能：** Phase 3 已收口；下一 Phase 尚未规划
- **分支：** main
- **说明：** feat-027/029~034 已完成；feat-028 独立初始基线入库经用户决定跳过。最终固定 GT usable 92.0%、CER 3.2%、timing F1 97.7%、precision 98.8%。

## 进行中

- 无

## 近期完成（最近 5 个）

- [x] **bugfix**：收回 GUI「SSIM 巡逻」开关（ADR-0013）；产品路径不传 `enable_ssim_patrol`，走后端默认 True；IPC 字段保留给诊断。
- [x] **feat-032**：Phase 3 文档、状态与最终质量口径完成收口。
- [x] **feat-030**：前台进度与快速取消，取消响应 0.108s，CLI/GUI 阶段百分比对应真实处理进度。
- [x] **feat-029**：增量架构验收，解决 Vision 内存泄漏，流式时机与 Cancel 达标。
- [x] **feat-034**：P1 修复与固定 GT 复测通过；报告见 `debug/benchmark-reports/feat034_p1_fix2/`。

## 阻塞项 / 风险

- [ ] **merged residual / #15 砰**：非 034 主目标，仍开放。
- [ ] **ground truth 素材有限**：主要依赖 Zootopia clip。
- [ ] **feat-030 长视频 GUI 手工验收暂缓**：尚未用 ≥10 分钟非 Zootopia 视频验证进度观感、取消和重新开始；自动 IPC 审计已通过。
- [ ] **显式 CJK 混排风险**：边界 cleanup 可能误删 `NPD动物警局` / `苹果的iPhone` 一类无空格英文；默认 auto，待混排 GT 驱动修复。

## 近期决策

- **ADR-0013**：SSIM patrol 为内部默认机制，不暴露给 GUI 用户。
- **ADR-0012**：增量 pipeline、真实进度、快速取消与 Vision 资源释放由同一任务生命周期管理。
- **ADR-0011**：默认 script=`auto`；显式 CJK 边界 cleanup 的混排风险保留为 HURDLE。

> 完整决策记录见 `docs/DECISIONS.md`
