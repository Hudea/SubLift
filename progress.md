# SubLift 进度导航

## 当前状态

- **最后更新：** 2026-08-12
- **当前 Phase：** Phase 10 macOS Native Workbench UI 正在进行；10001、10102、10103、10104、10105、10206、10207、10208 已完成。
- **进度真源：** `phases.json → detail_file`；本文件仅作会话导航。
- **下一优先：** 10209 实现 Review、Subtitle Inspector 与 Export（final entries 原子切换、Subtitle Inspector、SRT 导出主动作）。
- **当前计划：** `docs/plans/architecture/phase10-macos-workbench-ui.md`；主视觉与交互真源见 `docs/design_ui/`。

## 进行中

- 无正在实施的 Feature；等待启动 10209。

## 近期完成

- [x] 10208：安全的 Processing 体验已完成；真实进度/runtime 投影、Live Transcript 只读说明、Extraction Inspector（引擎/质量/运行时）、V05 截图证据见 `docs/phases/phase10.json`（processing 时刻截图因引擎过快未执行，如实记录）。
- [x] 10207：Transcript Panel 与上下文命令已完成；搜索过滤投影、行层级、selection/current、右键命令、V06b 截图证据见 `docs/phases/phase10.json`。
- [x] 10206：字幕区域编辑模式已完成；候选框仅 regionEditing、语义色+复选标记、Region Inspector（多选/重检/合并摘要/高级几何）、V04 截图证据见 `docs/phases/phase10.json`。
- [x] 10105：Context Inspector 与 Video 模式已完成；真实 metadata 字段、模式占位、metadataBar 迁移、V03 截图证据见 `docs/phases/phase10.json`。
- [x] 10104：Native Workspace Shell 已完成；Toolbar/菜单共用命令、62/38 Split、Inspector 骨架与预留回收、V03/V08 截图证据见 `docs/phases/phase10.json`。

## 阻塞项 / 风险

- Computer Use 视觉验收受系统权限限制：真实 Finder 拖拽、AX 树逐项验证等 UI 操作未执行（录屏/辅助功能权限警告持续存在，cua-driver 对多数窗口捕获返回 0x0）；已改用 EvidenceShot（app 内渲染）生成截图，逐 Feature 在 evidence 如实记录未执行项；交互逻辑由单测覆盖。后续 Feature 如需真实 UI 交互验收，需用户在系统设置中为终端/Hermes 授权"辅助功能"。
- 完整 `swift test` 前必须 `export PYTHONPATH=`（Hermes 会话 PYTHONPATH 污染 .venv 子进程会导致集成测试挂起）。
- 辅助 vision provider 当前拒绝图像输入（vision_analyze 不可用），截图验收改用程序化像素分析。

## 近期决策

- ADR-0035：Phase 10 采用无永久 Sidebar 的 Native Workbench、Context Inspector 与处理期只读 Transcript。
- ADR-0033：以 subtraction 模板精简活跃 Harness，校正运行时与历史 Phase 边界。
- ADR-0032：采用 Phase 索引与 legacy/canonical 兼容层；活跃 Harness 布局后由 ADR-0033 收缩。

> 10208 已完成；10209–10413 保持 `not-started`。
