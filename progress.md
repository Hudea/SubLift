# SubLift 进度导航

## 当前状态

- **最后更新：** 2026-08-12
- **当前 Phase：** Phase 10 macOS Native Workbench UI **已重新收口**（10001、10102、10103–10415 全部 done）。
- **进度真源：** `phases.json → detail_file`；本文件仅作会话导航。

## 近期完成

- [x] 10415：快速提取设置栏已完成；ExtractionProgressView 下方 52–64pt 紧凑栏（引擎 Popup + 采样分段 + 待生效提示 + 重新提取）、active/final 配置快照生命周期、统一 requestExtraction 与替换确认、Inspector 去重只读化、V09/V09b 截图证据见 `docs/phases/phase10.json`。
- [x] 10414：Phase 10 独立审计修复与真实 UI 验收完成；Mock fail-closed、Inspector 紧凑布局、first-responder Esc、源序号搜索及视觉细节均已修复，177+140 Swift 测试与标准门 10/10 通过，证据见 `docs/phases/phase10.json`。
- [x] 10413：Phase 10 收口已完成；全量审计（status/subtasks/evidence 非空）、旧 UI 零残留、无双重状态真源、文档同步（ARCHITECTURE/REQUIREMENTS）、最终验证（init / 158+140 测试 / verify-standard 10/10 / jq / diff-check / 链接扫描）全部通过，证据见 `docs/phases/phase10.json`。
- [x] 10412：响应式与辅助功能硬化已完成；快捷键目录+Esc 退出区域编辑、Light/Dark 外观矩阵 8 张截图、组合路径回归测试，证据见 `docs/phases/phase10.json`。
- [x] 10311：Settings 与引擎可见性已完成；General/Recognition/Advanced 三分区、Mock 仅开发者模式、capability 真实 inline、无假下载按钮、V07 截图证据见 `docs/phases/phase10.json`。

## 阻塞项 / 风险（如实记录）

- 完整 `swift test` 前必须 `export PYTHONPATH=`（Hermes 会话 PYTHONPATH 污染 .venv 子进程会导致集成测试挂起）。
- 完整 JSON Schema 校验未执行（仓库无 validator，未新增第三方依赖）；已做结构级核验。
- mock/vision 引擎对测试视频提取均生成 0 条（Worker 管线行为，未改动）；含条目的 UI 截图使用 DEBUG 注入 fixture。
- 真实 UI 交互（点击/键盘/VoiceOver）受系统"辅助功能访问（事件）"权限限制未执行；以单测 + 代码审核 + 渲染核验覆盖（逐 Feature 在 evidence 记录）。

## 近期决策

- ADR-0035：Phase 10 采用无永久 Sidebar 的 Native Workbench、Context Inspector 与处理期只读 Transcript。
- ADR-0033：以 subtraction 模板精简活跃 Harness，校正运行时与历史 Phase 边界。
- ADR-0032：采用 Phase 索引与 legacy/canonical 兼容层；活跃 Harness 布局后由 ADR-0033 收缩。

> **Phase 10 已重新收口**（10415 经 TDD、三路独立代码审核、修改复核、真实 macOS UI 截图与完整项目门验收）。
