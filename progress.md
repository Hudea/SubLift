# SubLift 进度导航

## 当前状态

- **最后更新：** 2026-08-11
- **当前 Phase：** Phase 10 macOS Native Workbench UI 正在进行；10001、10102 已完成。
- **进度真源：** `phases.json → detail_file`；本文件仅作会话导航。
- **下一优先：** 10103 Welcome 与视频导入状态。
- **当前计划：** `docs/plans/architecture/phase10-macos-workbench-ui.md`；主视觉与交互真源见 `docs/design_ui/`。

## 进行中

- 无正在实施的 Feature；等待启动 10103。

## 近期完成

- [x] 10102：Workspace Session 状态模型、命令矩阵、请求/任务隔离、处理期只读与失败/取消恢复已完成；证据见 `docs/phases/phase10.json`。
- [x] 10001：外部 vNext 方案与 8 张参考图已整理为主设计、状态合同、实现映射和 13 个 Phase 10 Feature；验证证据见 `docs/phases/phase10.json`。
- [x] Phase 9：仓库稳定化与清理已完整收口，证据见 `docs/phases/phase9.json`。
- [x] 09005：安全本地产物入口已支持默认 dry-run、分类选择和路径保护；未执行真实本机清理。
- [x] 09004：benchmark canonical CLI、历史 shim、root fallback 与版本化资产边界已收口。

## 阻塞项 / 风险

- 无当前阻塞。Phase 10 明确不实现 Task Center/批量队列、自动引擎、Whisper、ASS/VTT、模型下载或独立分发；processing 实时字幕必须保持只读，避免最终 entries 覆盖用户编辑。发布前仍须显式运行完整发布门。

## 近期决策

- ADR-0035：Phase 10 采用无永久 Sidebar 的 Native Workbench、Context Inspector 与处理期只读 Transcript。
- ADR-0033：以 subtraction 模板精简活跃 Harness，校正运行时与历史 Phase 边界。
- ADR-0032：采用 Phase 索引与 legacy/canonical 兼容层；活跃 Harness 布局后由 ADR-0033 收缩。

> Phase 10 已完成设计基线与 Workspace Session 状态模型；10103–10413 保持 `not-started`。
