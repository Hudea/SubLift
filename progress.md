# 会话进度日志

## 当前状态

- **最后更新：** 2026-07-10
- **当前 Phase：** Phase 3 - 优化与基本可用
- **当前功能：** feat-034 已完成；下一功能未启动
- **分支：** opt/ocr-timeline
- **说明：** feat-034 P1 已收口；固定 GT 达 usable 92.0%、CER 3.2%、noise/empty 0、timing F1 97.7%、precision 98.8%。

## 进行中

- 无

## 近期完成（最近 5 个）

- [x] **feat-034**：P1 修复与固定 GT 复测通过；报告见 `debug/benchmark-reports/feat034_p1_fix2/`。
- [x] path mode / ADR-0010 统一抽帧。
- [x] feat-033：打轴 residual。F1 95.2%。
- [x] feat-031：SSIM patrol。

## 阻塞项 / 风险

- [ ] **feat-028 跳过**：基线对比用报告目录。
- [ ] **merged residual / #15 砰**：非 034 主目标，仍开放。
- [ ] **ground truth 素材有限**：主要依赖 Zootopia clip。

## 近期决策

- **feat-034**：默认 script=`auto`；GUI 按选中文字推断；固定中文 benchmark 显式 `cjk`。
- **feat-034 cleanup**：只清理显式 CJK 下与中文边界直接粘连的拉丁横幅，保留合法英文与内部混排。
- **feat-034b**：profile 几何相对 crop。

> 完整决策记录见 `docs/DECISIONS.md`
