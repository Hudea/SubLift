# SubLift 进度导航

## 当前状态

- **最后更新：** 2026-08-12
- **当前 Phase：** Phase 10 macOS Native Workbench UI 正在进行；10001、10102、10103 已完成。
- **进度真源：** `phases.json → detail_file`；本文件仅作会话导航。
- **下一优先：** 10104 Native Workspace Shell（Toolbar、Video/Transcript Split、Menu Commands、960×600 响应式）。
- **当前计划：** `docs/plans/architecture/phase10-macos-workbench-ui.md`；主视觉与交互真源见 `docs/design_ui/`。

## 进行中

- 无正在实施的 Feature；等待启动 10104。

## 近期完成

- [x] 10103：Welcome 与视频导入状态已完成；校验 fail-closed、Open/drop 同一 intent、V01/V02 截图证据见 `docs/phases/phase10.json`。
- [x] 10102：Workspace Session 状态模型、命令矩阵、请求/任务隔离、处理期只读与失败/取消恢复已完成；证据见 `docs/phases/phase10.json`。
- [x] 10001：外部 vNext 方案与 8 张参考图已整理为主设计、状态合同、实现映射和 13 个 Phase 10 Feature；验证证据见 `docs/phases/phase10.json`。
- [x] Phase 9：仓库稳定化与清理已完整收口，证据见 `docs/phases/phase9.json`。
- [x] 09005：安全本地产物入口已支持默认 dry-run、分类选择和路径保护；未执行真实本机清理。

## 阻塞项 / 风险

- Computer Use 视觉验收受系统权限限制：真实 Finder 拖拽、Open Panel 等 UI 操作未执行（录屏/辅助功能权限警告），已在 10103 evidence 如实记录；drop/导入逻辑由单测覆盖。后续 Feature 如需真实 UI 交互验收，需用户在系统设置中为终端/Hermes 授权"辅助功能"。
- 完整 `swift test` 前必须 `export PYTHONPATH=`（Hermes 会话 PYTHONPATH 污染 .venv 子进程会导致集成测试挂起）。

## 近期决策

- ADR-0035：Phase 10 采用无永久 Sidebar 的 Native Workbench、Context Inspector 与处理期只读 Transcript。
- ADR-0033：以 subtraction 模板精简活跃 Harness，校正运行时与历史 Phase 边界。
- ADR-0032：采用 Phase 索引与 legacy/canonical 兼容层；活跃 Harness 布局后由 ADR-0033 收缩。

> 10103 已完成；10104–10413 保持 `not-started`。
