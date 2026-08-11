# SubLift 进度导航

## 当前状态

- **最后更新：** 2026-08-12
- **当前 Phase：** Phase 10 macOS Native Workbench UI **已完整收口**（10001、10102、10103–10413 全部 done）。
- **进度真源：** `phases.json → detail_file`；本文件仅作会话导航。

## 近期完成

- [x] 10413：Phase 10 收口已完成；全量审计（status/subtasks/evidence 非空）、旧 UI 零残留、无双重状态真源、文档同步（ARCHITECTURE/REQUIREMENTS）、最终验证（init / 158+140 测试 / verify-standard 10/10 / jq / diff-check / 链接扫描）全部通过，证据见 `docs/phases/phase10.json`。
- [x] 10412：响应式与辅助功能硬化已完成；快捷键目录+Esc 退出区域编辑、Light/Dark 外观矩阵 8 张截图、组合路径回归测试，证据见 `docs/phases/phase10.json`。
- [x] 10311：Settings 与引擎可见性已完成；General/Recognition/Advanced 三分区、Mock 仅开发者模式、capability 真实 inline、无假下载按钮、V07 截图证据见 `docs/phases/phase10.json`。
- [x] 10310：Subtitle Timeline 与播放导航已完成；32pt 确定性时间几何、最小命中宽度、点击 seek、Previous/Next 复用命令、V06b 截图证据见 `docs/phases/phase10.json`。
- [x] 10209：Review、Subtitle Inspector 与 Export 已完成；Subtitle Inspector 真实字段、Review 可编辑、导出仅 review 且 SRT、V06 截图证据见 `docs/phases/phase10.json`。

## 阻塞项 / 风险（Phase 10 遗留，如实记录）

- Computer Use 视觉验收受系统权限限制：真实 Finder 拖拽、AX 树逐项验证、VoiceOver 会话等 UI 操作未执行（录屏/辅助功能权限警告持续存在，cua-driver 对多数窗口捕获返回 0x0）；已用 EvidenceShot（app 内渲染）生成截图（含 Light/Dark 矩阵），逐 Feature 在 evidence 如实记录未执行项；交互逻辑由单测覆盖。后续如需真实 UI 交互验收，需用户在系统设置中为终端/Hermes 授权"辅助功能"。
- 完整 `swift test` 前必须 `export PYTHONPATH=`（Hermes 会话 PYTHONPATH 污染 .venv 子进程会导致集成测试挂起）。
- 完整 JSON Schema 校验未执行（仓库无 validator，未新增第三方依赖）；已做结构级核验。
- mock/vision 引擎对测试视频提取均生成 0 条（Worker 管线行为，未改动）；含条目的 UI 截图使用 DEBUG 注入 fixture。

## 近期决策

- ADR-0035：Phase 10 采用无永久 Sidebar 的 Native Workbench、Context Inspector 与处理期只读 Transcript。
- ADR-0033：以 subtraction 模板精简活跃 Harness，校正运行时与历史 Phase 边界。
- ADR-0032：采用 Phase 索引与 legacy/canonical 兼容层；活跃 Harness 布局后由 ADR-0033 收缩。

> **Phase 10 已收口**（12 个实施 Feature 原子提交 + 10413 审计通过）。
