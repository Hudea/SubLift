# 会话进度日志

## 当前状态

- **最后更新：** 2026-07-12
- **当前 Phase：** Phase 3 - 优化与基本可用
- **当前功能：** feat-030 已完成；下一功能未启动
- **分支：** opt/ocr-timeline
- **说明：** feat-030 前台进度显示与快速取消收口。取消延迟低至 0.10s。

## 进行中

- 无

## 近期完成（最近 5 个）

- [x] **feat-030**：前台进度与快速取消，取消响应极速（0.1s），CLI/GUI 阶段百分比精准对应。
- [x] **feat-029**：增量架构验收，解决 Vision 内存泄漏，流式时机与 Cancel 达标。
- [x] **feat-034**：P1 修复与固定 GT 复测通过；报告见 `debug/benchmark-reports/feat034_p1_fix2/`。
- [x] path mode / ADR-0010 统一抽帧。
- [x] feat-033：打轴 residual。F1 95.2%。

## 阻塞项 / 风险

- [ ] **feat-028 跳过**：基线对比用报告目录。
- [ ] **merged residual / #15 砰**：非 034 主目标，仍开放。
- [ ] **ground truth 素材有限**：主要依赖 Zootopia clip。
- [ ] **feat-030 长视频 GUI 手工验收暂缓**：尚未用 ≥10 分钟非 Zootopia 视频验证进度观感、取消和重新开始；自动 IPC 审计已通过。

## 近期决策

- **feat-034**：默认 script=`auto`；GUI 按选中文字推断；固定中文 benchmark 显式 `cjk`。
- **feat-034 cleanup**：只清理显式 CJK 下与中文边界直接粘连的拉丁横幅，保留合法英文与内部混排。
- **feat-034b**：profile 几何相对 crop。

> 完整决策记录见 `docs/DECISIONS.md`
