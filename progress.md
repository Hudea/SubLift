# 会话进度日志

## 当前状态

- **最后更新：** 2026-07-06
- **当前 Phase：** Phase 3 - 优化与基本可用
- **当前功能：** feat-027 Benchmark 框架
- **分支：** main
- **说明：** Phase 3 规划已建立，目标限定为三项：benchmark 优化、增量处理与前台进度优化、打轴检测优化。OCR 区域裁剪、ASS/VTT、PaddleOCR、.app 打包等明确后置。

## 进行中

- [ ] feat-027：建立端到端 benchmark 框架（指标定义、ground truth 加载、一键运行、报告输出）。

## 近期完成（最近 5 个）

- [x] Phase 3 规划文档建立：`docs/plans/phase3.md`、`docs/phases/phase3.json`，更新 `feature-list.json`。
- [x] `apps/macos-gui` 分支合并到 `main`：Phase 2 所有计划内功能完成，feat-025 跳过。
- [x] feat-026：Phase 2 文档收尾全部完成。
- [x] feat-024：SRT 导出（Swift 端直接格式化 + NSSavePanel）。117 Swift 测试全绿。
- [x] feat-023：引擎选择（SettingsView + UserDefaults）。

## 阻塞项 / 风险

- [ ] **Phase 3 范围控制**：用户明确先不规划具体实现方法，需在每个 feat 启动时重新评估技术方案，避免范围发散。
- [ ] **增量处理重构风险**：允许修改 pipeline/，可能引入回归；必须先完成 benchmark 基线再动手重构。
- [ ] **ground truth 素材有限**：目前主要依赖 Zootopia clip，benchmark 说服力可能不足。

## 近期决策

- Phase 3 目标限定为：benchmark 优化、增量处理 + 前台进度优化、打轴检测优化。
- Phase 3 允许重构 `src/sublift/pipeline/` 与 `src/sublift/ipc/bridge.py`（解除 Phase 2 的零修改约束）。
- OCR 区域裁剪、ASS/VTT 导出、PaddleOCR、CLI 配置文件、`.app` 打包明确后置。

> 完整决策记录见 `docs/DECISIONS.md`
