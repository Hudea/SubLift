# SubLift 进度导航

## 当前状态

- **最后更新：** 2026-08-12
- **当前 Phase：** Phase 8 批量任务中心已完成 08001 设计/架构基线；下一 Feature 为 08102 批量任务领域模型。Phase 7 与 Phase 10 已收口，Phase 9 编号仍可供其他新范围使用。
- **进度真源：** `phases.json → detail_file`；本文件仅作会话导航。

## 当前计划

- [Phase 8 批量任务队列与文件夹导入](docs/plans/architecture/phase8-batch-task-center.md)：按 08102 → 08103/08104 → 08205 → 08206 → 08207 → 08308 → 08309 → 08410 顺序实施。

## 近期完成

- [x] 08001：Phase 8 产品合同、双组合根/串行调度/安全输出/JSON 恢复架构与 08102–08410 验收拆解已冻结；实现尚未开始，证据见 `docs/phases/phase8.json`。
- [x] 10416：Phase 10 当前文档与计划状态已统一到 10415 最终实现；201+140 Swift 测试、标准门 10/10、JSON Schema、依赖/状态、318 个本地链接、diff check 与 init 全部通过，并与 07002 台账迁移共同组成最终纯文档提交。
- [x] 07002：原 Phase 9 的 09001–09006 已无损吸收到 Phase 7 项目辅助架构；历史 ID/evidence 保留，Phase 9 索引与详情路径已释放，未来从 09101 起登记。
- [x] 10415：快速提取设置栏已完成；ExtractionProgressView 下方 52–64pt 紧凑栏（引擎 Popup + 采样分段 + 待生效提示 + 重新提取）、active/final 配置快照生命周期、统一 requestExtraction 与替换确认、Inspector 去重只读化、V09/V09b 截图证据见 `docs/phases/phase10.json`。
- [x] 10414：Phase 10 独立审计修复与真实 UI 验收完成；Mock fail-closed、Inspector 紧凑布局、first-responder Esc、源序号搜索及视觉细节均已修复，177+140 Swift 测试与标准门 10/10 通过，证据见 `docs/phases/phase10.json`。

## 阻塞项 / 风险（如实记录）

- 完整 `swift test` 前必须 `export PYTHONPATH=`（Hermes 会话 PYTHONPATH 污染 .venv 子进程会导致集成测试挂起）。
- mock/vision 引擎对测试视频提取均生成 0 条（Worker 管线行为，未改动）；含条目的 UI 截图使用 DEBUG 注入 fixture。
- 真实 UI 交互（点击/键盘/VoiceOver）受系统"辅助功能访问（事件）"权限限制未执行；以单测 + 代码审核 + 渲染核验覆盖（逐 Feature 在 evidence 记录）。

## 近期决策

- ADR-0037：Phase 8 采用独立 Task Center + 单并发队列；入队配置快照、SRT 安全写入与 JSON 显式恢复，不扩张 Workspace 或 Worker/IPC。
- ADR-0036：项目辅助架构统一归入 Phase 7，保留 090xx 历史 ID 并释放 Phase 9；未来新 Phase 9 从 09101 起登记。
- ADR-0035：Phase 10 采用无永久 Sidebar 的 Native Workbench、Context Inspector 与处理期只读 Transcript。

> **Phase 8 当前只完成规划 Feature 08001**：F24 仍未实现；不得因 Task Center 参考图或计划文档宣称批量产品能力已经交付。
