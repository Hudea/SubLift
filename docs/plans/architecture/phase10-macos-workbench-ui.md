---
route: architecture-first
plan_type: architecture
status: ready
planning_level: L2
source: "用户提供外部 SubLift vNext 设计说明与 8 张参考图，要求整理设计并拆解 Phase 10"
created: 2026-08-11
tracking: "设计基线 10001 已完成；产品实施从 10102 开始，必须一次只启动一个 Feature"
---

# 架构计划：Phase 10 macOS Native Workbench UI

## 目标

在不改变字幕算法、IPC 和默认 runtime 的前提下，把现有 SwiftUI 开发者 GUI 升级为原生 macOS 字幕工作台：以视频、字幕和当前任务为主，使用 Toolbar、Split View、Context Inspector、Settings 与 Menu Commands 组织真实能力，并消除处理期编辑可能被最终结果覆盖的 UX 风险。

主设计真源为 [`docs/design_ui/`](../../design_ui/README.md)。本计划只定义实施顺序、架构边界、验证策略和停止条件。

## 当前证据

- `ContentView` 直接持有 Player、Extractor、Editor、Metadata、Region 等对象，适合 MVP，但会让多状态 View 继续膨胀。
- 当前 UI 已具备导入、预览/seek、候选区、实时 push、最终 entries、编辑、取消、SRT 导出、质量与引擎偏好。
- `SubtitleExtractor.entries` 在 processing 中增量更新，完成时替换为最终 entries；`SubtitleEditor` 目前可随时修改，存在“最终 load 覆盖处理期编辑”的交互风险。
- `RuntimePolicy` 已冻结 C++ 默认、Python 显式回滚、capability 缺失 fail-closed；Phase 10 只能展示和调用该策略。
- F10 自动引擎、F24 批量队列、ASS/VTT、独立分发仍未完成，不能因参考图出现而纳入本轮。

## 架构边界

### 新增协调层

`WorkspaceModel` 拥有单窗口 Session 协调和 command availability；focused Core models 继续拥有播放、提取、编辑、metadata、region、runtime 与导出逻辑。View 只投影状态和发送 intent。

### 原生 Shell

- `WorkspaceRootView` 组合 Welcome、Video Workspace、Transcript 与可选 Inspector。
- Toolbar/Menu/Context menu 复用统一 commands。
- Inspector 以 Video / Region / Extraction / Subtitle 四种上下文投影现有数据。
- Settings 只存放跨 Session 偏好；Mock 和 Python Oracle 退到 Advanced/Developer。

### 处理期安全

processing/finalizing 的 Live Transcript 是只读投影；final entries 完成替换后才构造可编辑 Review。取消、错误、重试和迟到回调由 Workspace 与现有 job token 共同约束。

## Feature 顺序

1. **10001 设计基线**：整理方案、资产、状态合同、实现映射和 Phase 跟踪（本次完成）。
2. **10102 Session 状态模型**：建立 WorkspaceModel/State 和统一 command availability。
3. **10103 Welcome 与导入**：空状态、Open、合法/非法拖入、加载与新视频切换。
4. **10104 Native Shell**：Toolbar、Video/Transcript Split、菜单与响应式基础。
5. **10105 Inspector 基础**：容器、模式切换与 Video metadata。
6. **10206 Region Editing**：显式模式、统一 overlay、Region Inspector 与重检。
7. **10207 Transcript Panel**：搜索、行层级、selection/current、上下文命令。
8. **10208 Processing 安全**：真实进度、runtime、Stop、Live Transcript 只读、取消/错误恢复。
9. **10209 Review 与字幕 Inspector**：最终可编辑切换、confidence、导出主动作。
10. **10310 Subtitle Timeline**：轻量字幕轨、播放头、点击 seek 与前后字幕命令。
11. **10311 Settings 分层**：General/Recognition/Advanced、Mock/Oracle 隔离、可用性状态。
12. **10412 适配与辅助功能**：960×600、Dark、Contrast、Transparency、VoiceOver、Keyboard。
13. **10413 Phase 收口**：全量 Swift/项目门、视觉证据矩阵、文档和状态同步。

Feature 的逐项 acceptance、subtask、依赖和 evidence 格式见 [`phase10.json`](../../phases/phase10.json)。计划 ready 不代表 10102–10413 已实施或授权批量推进。

## 验证策略

| 改动 | 证据 |
|---|---|
| Workspace 状态/commands | 新 Swift model tests 覆盖全转换、取消、失败、迟到事件与 enabled matrix |
| Shell/Inspector/Transcript/Timeline | accessibility identifiers + 确定性 View fixture + 对应尺寸实际截图 |
| Runtime/提取接线 | 既有 RuntimePolicy/PipelineClient/SubtitleExtractor 测试和新只读安全测试 |
| Region | 既有坐标/Selection tests + 编辑模式/重检/overlay 可达性测试 |
| Settings | UserDefaults 隔离 fixture、Vision/Paddle/Mock/Runtime policy 测试 |
| 每个 Feature | `cd apps/macos && swift test`；与改动相关的专项测试和截图 |
| Phase 收口 | `./init.sh`、`./scripts/verify-standard.sh`、JSON schema、Markdown links、V01–V10/A01–A02 |

## 非目标

- 不实现 Task Center、批量队列、Whisper、新 OCR Provider 或自动引擎路由。
- 不实现 ASS/VTT、模型下载、Session 持久化或复杂时间线编辑。
- 不改 C++/Python pipeline、UDS framing、Worker path mode、ROI/坐标语义或 fail-closed 策略。
- 不做 `.app` 分发、签名、公证或 CI/CD。
- 不合并或恢复未合入 main 的 Phase 8 UI 分支；它只能作为独立历史参考，不能覆盖本次基线。

## 停止与回滚条件

- 若重构要求修改 IPC/算法才能维持 UI，应停止当前 Feature，先登记独立架构任务。
- 若 processing 中开放任何 mutation，应停止并先证明最终 entries 不会覆盖编辑；默认保持只读。
- 若 Automatic、Python fallback、模型安装或新格式没有真实产品契约，应隐藏而非造假。
- 每个 Feature 保持 Swift 工程可构建测试；替代 View 接线完成前不删除旧实现。
- 视觉参考与 macOS 13 系统组件冲突时优先系统语义和可访问性，并在 evidence 记录偏差。

## Architecture Gate

- [x] 主设计、状态、组件和资产基线明确
- [x] 当前能力与参考图未来能力已分离
- [x] Workspace 协调层与 focused Core 依赖方向明确
- [x] Feature 顺序、验证证据和停止条件明确
- [x] Phase 10 已可从 10102 开始逐项开发
