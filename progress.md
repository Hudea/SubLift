# SubLift 进度导航

## 当前状态

- **最后更新：** 2026-08-12
- **当前 Phase：** Phase 7 项目辅助架构与 Phase 10 Native Workbench UI 均已收口；当前没有进行中的 Phase，Phase 9 编号可供新范围使用。
- **进度真源：** `phases.json → detail_file`；本文件仅作会话导航。

## 近期完成

- [x] 10416：Phase 10 当前文档与计划状态已统一到 10415 最终实现；201+140 Swift 测试、标准门 10/10、JSON Schema、依赖/状态、318 个本地链接、diff check 与 init 全部通过，并与 07002 台账迁移共同组成最终纯文档提交。
- [x] 07002：原 Phase 9 的 09001–09006 已无损吸收到 Phase 7 项目辅助架构；历史 ID/evidence 保留，Phase 9 索引与详情路径已释放，未来从 09101 起登记。
- [x] 10415：快速提取设置栏已完成；ExtractionProgressView 下方 52–64pt 紧凑栏（引擎 Popup + 采样分段 + 待生效提示 + 重新提取）、active/final 配置快照生命周期、统一 requestExtraction 与替换确认、Inspector 去重只读化、V09/V09b 截图证据见 `docs/phases/phase10.json`。
- [x] 10414：Phase 10 独立审计修复与真实 UI 验收完成；Mock fail-closed、Inspector 紧凑布局、first-responder Esc、源序号搜索及视觉细节均已修复，177+140 Swift 测试与标准门 10/10 通过，证据见 `docs/phases/phase10.json`。
- [x] 10413：Phase 10 收口已完成；全量审计（status/subtasks/evidence 非空）、旧 UI 零残留、无双重状态真源、文档同步（ARCHITECTURE/REQUIREMENTS）、最终验证（init / 158+140 测试 / verify-standard 10/10 / jq / diff-check / 链接扫描）全部通过，证据见 `docs/phases/phase10.json`。

## 阻塞项 / 风险（如实记录）

- 完整 `swift test` 前必须 `export PYTHONPATH=`（Hermes 会话 PYTHONPATH 污染 .venv 子进程会导致集成测试挂起）。
- mock/vision 引擎对测试视频提取均生成 0 条（Worker 管线行为，未改动）；含条目的 UI 截图使用 DEBUG 注入 fixture。
- 真实 UI 交互（点击/键盘/VoiceOver）受系统"辅助功能访问（事件）"权限限制未执行；以单测 + 代码审核 + 渲染核验覆盖（逐 Feature 在 evidence 记录）。

## 近期决策

- ADR-0036：项目辅助架构统一归入 Phase 7，保留 090xx 历史 ID 并释放 Phase 9；未来新 Phase 9 从 09101 起登记。
- ADR-0035：Phase 10 采用无永久 Sidebar 的 Native Workbench、Context Inspector 与处理期只读 Transcript。
- ADR-0033：以 subtraction 模板精简活跃 Harness，校正运行时与历史 Phase 边界。

> **Phase 10 已最终收口到 10416**：产品实现止于 10415；10416 完成当前文档、验证证据与合入准备，未扩大产品范围。
