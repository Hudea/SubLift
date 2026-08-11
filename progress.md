# SubLift 进度导航

## 当前状态

- **最后更新：** 2026-08-11
- **当前 Phase：** Phase 10 macOS Native Workbench UI 已登记；10001 设计基线完成，产品实现尚未开始。
- **进度真源：** `phases.json → detail_file`；本文件仅作会话导航。
- **下一优先：** 10102 Workspace Session 状态模型；等待用户进入实际开发流程后再启动。
- **当前计划：** `docs/plans/architecture/phase10-macos-workbench-ui.md`；主视觉与交互真源见 `docs/design_ui/`。

## 进行中

- 无正在实施的 Feature；10102 尚未启动。

## 近期完成

- [x] 10001：外部 vNext 方案与 8 张参考图已整理为主设计、状态合同、实现映射和 13 个 Phase 10 Feature；验证证据见 `docs/phases/phase10.json`。
- [x] Phase 9：仓库稳定化与清理已完整收口，证据见 `docs/phases/phase9.json`。
- [x] 09005：安全本地产物入口已支持默认 dry-run、分类选择和路径保护；未执行真实本机清理。
- [x] 09004：benchmark canonical CLI、历史 shim、root fallback 与版本化资产边界已收口。
- [x] 09003：根目录手工脚本已审计，独有诊断已迁入专用边界，冗余入口已删除。

## 阻塞项 / 风险

- 无当前阻塞。Phase 10 明确不实现 Task Center/批量队列、自动引擎、Whisper、ASS/VTT、模型下载或独立分发；processing 实时字幕必须保持只读，避免最终 entries 覆盖用户编辑。发布前仍须显式运行完整发布门。

## 近期决策

- ADR-0035：Phase 10 采用无永久 Sidebar 的 Native Workbench、Context Inspector 与处理期只读 Transcript。
- ADR-0033：以 subtraction 模板精简活跃 Harness，校正运行时与历史 Phase 边界。
- ADR-0032：采用 Phase 索引与 legacy/canonical 兼容层；活跃 Harness 布局后由 ADR-0033 收缩。

> Phase 10 设计基线已完成；10102–10413 均保持 `not-started`，等待进入实际开发流程。
