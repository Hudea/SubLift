---
route: architecture-first
plan_type: architecture
status: ready
planning_level: L2
source: "用户要求在当前分支规划 Phase 8，实现任务队列与文件夹支持"
created: 2026-08-12
tracking: "Phase 8 已全部完成（08001–08410，2026-08-13 综合验收收口）"
---

# 架构计划：Phase 8 批量任务队列与文件夹导入

## 目标

在不修改字幕算法、UDS framing 和默认 runtime 的前提下，为 macOS GUI 增加独立 Task Center：
接受多个本地视频或文件夹，将其转换为可恢复、可取消、错误隔离的串行任务队列，并安全输出
SRT。主设计合同见 [`batch-task-center.md`](../../design_ui/batch-task-center.md)，操作跟踪见
[`phase8.json`](../../phases/phase8.json)。

## 当前基线

- `WorkspaceModel` 是单窗口、单视频 Session 组合根，不适合同时拥有多任务历史和跨启动恢复。
- `SubtitleExtractor` 已实现每任务独占 `PipelineClient`、job token、真实 progress、快速取消和
  迟到更新隔离，可作为批量 Runner 的行为基线，但当前 API 通过 `@Published` 状态服务单任务 UI。
- `VideoImportPolicy` 已统一 MP4/MOV/MKV 与 MKV/ffmpeg fail-closed，可被文件夹扫描器复用。
- `ExtractionConfiguration` 已冻结 engine + sampling quality；Settings 与快速设置栏共用偏好，
  active/final 快照解决运行中配置漂移。
- `SrtFormatter` 已在 Swift 端输出 SRT，但当前由用户通过 `NSSavePanel` 手动选择单个输出。
- `PipelineClient`/Worker 同时启动多个实例在协议上可行，但 Phase 8 没有资源、质量或调度证据支持
  并行 OCR，因此首版固定串行。

## 架构边界

### 1. 双组合根

```text
SubLiftMacApp
├── WorkspaceModel              # 单视频预览、区域、提取、校对、手动导出
└── BatchQueueModel             # 多任务清单、队列命令、持久化与 Task Center 投影
      ├── BatchInputScanner     # 文件/目录展开、验证、去重、拒绝报告
      ├── BatchOutputPlanner    # sidecar/公共目录、相对路径、冲突策略
      ├── BatchQueueScheduler   # 单并发调度、暂停/停止、token 与错误隔离
      ├── BatchExtractionRunner # 单任务 progress/final/cancel 事件边界
      └── BatchQueueRepository  # versioned JSON 原子保存与恢复
```

`WorkspaceModel` 和 `BatchQueueModel` 不互相持有，也不共享可变 `SubtitleExtractor`。它们只共享
无状态策略、值类型和 Runner 协议。Task Center 的运行不能清空或替换当前 Workspace Session。

### 2. Domain

建议新增纯值类型：

```swift
enum BatchTaskStatus: Codable, Equatable {
    case waiting, preparing, extracting, exporting
    case completed, failed, cancelled, interrupted, skipped
}

struct BatchTask: Identifiable, Codable, Equatable {
    let id: UUID
    let sourceURL: URL
    var configuration: ExtractionConfiguration
    var outputPlan: BatchOutputPlan
    var status: BatchTaskStatus
    var progress: BatchTaskProgress?
    var result: BatchTaskResult?
    var failure: BatchTaskFailure?
    let addedAt: Date
}
```

持久化要求 `ExtractionConfiguration`、`OcrEngineName` 与 `SamplingQuality` 获得最小 Codable
能力；不要为持久化复制第二套 engine/quality string 真源。运行 token 不持久化。

### 3. Ports 与依赖方向

```swift
protocol BatchTaskRunning {
    func run(_ request: BatchRunRequest) -> AsyncThrowingStream<BatchRunEvent, Error>
    func cancel(taskID: UUID) async
}

protocol BatchQueuePersisting {
    func load() throws -> BatchQueueSnapshot?
    func save(_ snapshot: BatchQueueSnapshot) throws
}
```

- Scheduler 只依赖 `BatchTaskRunning`、`BatchQueuePersisting` 和时钟/UUID 等可注入能力；测试使用 fake。
- 生产 Runner 在内部为每项创建独占的 `PipelineClient`，复用当前 `SubtitleExtractor` 的 token、
  progress、final entries 和 teardown 语义；不让 Scheduler 解析 UDS 消息。
- UI 只观察 `BatchQueueModel` 的只读任务投影并发送 intent，不直接启动 Worker、扫描目录或写文件。
- Repository 只持久化 domain snapshot，不持久化 View 状态、完整 entries 或 Worker 对象。

### 4. 单任务执行数据流

```text
Scheduler 取首个 waiting task
  ↓ taskID + runToken
OutputPlanner 解析最终目标与冲突策略
  ├─ skip → skipped
  └─ runnable → preparing
  ↓
BatchExtractionRunner 创建独占 PipelineClient
  ↓ start_job(video_path, region_box=nil, engine, fps)
Worker progress → extracting + bounded progress snapshot
  ↓ final entries
SrtFormatter.format(entries) → atomic writer → exporting
  ↓ 成功后丢弃 entries
completed(entryCount, outputURL, runtimeIdentity)
  ↓
Repository 原子保存 → Scheduler 决定下一项或 paused
```

错误在 task 边界转换为稳定的 `BatchTaskFailure` 摘要；完整诊断继续写既有日志，不把无限日志文本
塞进队列清单。

### 5. 扫描与路径安全

- `BatchInputScanner` 使用 Foundation URL/resource values，不通过 shell 拼接或 glob 扫描。
- 对目录遍历结果做 `standardizedFileURL` 去重；不通过 `resolvingSymlinksInPath` 跟随软链接。
- 文件夹递归默认 false；递归时跳过 hidden、package 和 symbolic link。
- 公共输出目录模式使用输入 root 的相对路径；所有生成目标必须经过标准化后确认仍位于 output root，
  防止 `..` 或异常文件名逃逸。
- 扫描、metadata 和 output preflight 不占用 OCR Worker；不可读/不可写错误按文件隔离。

### 6. 调度与取消不变量

1. 任意时刻最多一个 task 处于 preparing/extracting/exporting。
2. 活动任务结束和 Runner teardown 完成之前不启动下一项。
3. pause 只设置 `pauseRequested`；活动任务完成后进入 paused。
4. stop/cancel 使当前 run token 失效，等待 Runner 明确结束或超时清理后停止调度。
5. taskID/runToken 不匹配的 progress/final/error 一律丢弃。
6. 失败默认继续；只有队列级持久化损坏或内部不变量破坏才停止整个队列。
7. Settings 改变不影响现有 task；waiting task 只有显式 edit intent 才替换配置。

### 7. 持久化与恢复

`BatchQueueSnapshot(schemaVersion: 1, queueState, tasks, savedAt)` 写入 Application Support 下
`SubLift/batch-queue-v1.json`。保存频率限定在结构/状态变化及节流后的进度 checkpoint，避免每帧
写盘。恢复规则：

- `preparing/extracting/exporting` → `interrupted`；
- queue state → `paused`；
- completed/failed/cancelled/skipped/waiting 保持；
- 不自动拉起 Worker；
- schema 版本未知或 JSON 损坏时 fail-closed 并保留原文件。

本阶段不新增数据库，也不实现 App Sandbox bookmark。若 JSON 清单增长到影响启动，再以真实数据
登记压缩/分页 Feature，不预先引入 SQLite。

### 8. UI Scene 与命令

`SubLiftMacApp` 在现有 Workspace `WindowGroup` 和 Settings 之外增加 Task Center Window/Scene，
App 层持有唯一 `BatchQueueModel`。主窗口 Toolbar/Menu 提供“显示任务中心”，不在 Workspace
增加永久队列栏。Task Center 的 Toolbar、Menu 和行上下文菜单必须复用统一 command availability。

## Feature 顺序

1. **08001 设计与架构基线**：产品合同、架构边界、Feature/验收拆解和文档同步。
2. **08102 任务领域模型**：Codable task/status/result/failure、状态转换和配置快照。
3. **08103 文件与文件夹发现**：多入口扫描、递归、去重、拒绝报告和 VideoImportPolicy 复用。
4. **08104 输出规划与冲突保护**：sidecar/公共目录、相对路径、skip/rename/replace 与原子写入。
5. **08205 串行调度器**：单并发、pause/stop/cancel/retry/reorder、token 和错误隔离。
6. **08206 真实 Runner 与导出接线**：PipelineClient 生命周期、事件流、SRT 写入和 Workspace 回归。
7. **08207 队列持久化与恢复**：versioned JSON、节流保存、interrupted 恢复和损坏清单处理。
8. **08308 Task Center Window**：原生 Table/List、Toolbar、Detail、summary 与真实状态。
9. **08309 批量交互与辅助功能**：拖入、搜索、筛选、多选、重排、Finder、键盘和 VoiceOver。
10. **08410 Phase 8 综合验收**：真实组合路径、长队列/资源、独立审核、文档与完整项目门。

逐 Feature 的模块、subtask、acceptance、non-goal 和依赖以
[`phase8.json`](../../phases/phase8.json) 为准。

## TDD 与验证策略

| 层 | 必须证据 |
|---|---|
| Domain | 状态转换表、Codable 往返、配置锁定、run token 纯逻辑测试 |
| Scanner | 临时目录覆盖递归开关、大小写扩展名、隐藏/package/symlink、去重、权限/空目录 |
| Output | 路径逃逸、同名冲突、skip/rename/replace、原文件保护和临时文件清理 |
| Scheduler | Fake Runner 证明 max active=1、pause/stop/cancel/retry、失败继续、迟到事件隔离 |
| Runner | fake IPC/既有 integration + 至少两个实际小视频串行 smoke；验证 teardown 后再启动下一项 |
| Repository | 临时目录保存/恢复、未知版本、损坏 JSON、active→interrupted、100 task 体积/恢复时间 |
| UI | B01–B10 fixture、1280/960、Light/Dark、长路径、真实 command availability 与 AX labels |
| Phase | 完整 Swift、标准门、schema、链接、diff-check、init 和真实文件夹组合验收 |

截图与真实 UI 记录按现有约定分别写入 `docs/design_ui/evidence/<feature-id>/`；08308、08309
和 08410 不共用一张图冒充多个不同状态或交互验收。

每个实现 Feature 必须按“计划 → RED → 实现 → GREEN → 独立审核 → 修改 → 复核 → 真实验收 →
evidence → 原子提交”的顺序闭环，不能积攒多个 Feature 后一起验收。

## 非目标

- 不并行多个 Worker，不新增并发数设置或资源自动调度；
- 不做目录监听、后台 daemon、网络/云同步或跨设备队列；
- 不做 Automatic/Whisper/新 Provider、ASS/VTT、翻译或软字幕；
- 不做逐任务交互式 Region Editing、批量字幕编辑或 Workspace Session 持久化；
- 不修改 C++/Python pipeline、OCR、UDS framing、runtime/fail-closed 和 parity/golden；
- 不新增第三方依赖、数据库、CI/CD、分发、签名、公证或 App Sandbox。

## 停止条件

- 若必须修改 Worker/UDS 才能表达队列，停止当前 Feature，登记独立协议迁移并先征得用户同意。
- 若实现需要同时运行多个 Worker，停止并保留串行；并发必须另有资源和质量证据。
- 若无法在不覆盖原文件的情况下完成输出，任务应 failed，不得用“完成”掩盖写入失败。
- 若真实 Runner 无法可靠 teardown/restart，不进入 UI Feature，先在 08206 修复并复验。
- 若 960×600 无法同时保留主控制和任务状态，优先隐藏低优先级列，不缩小到不可读字号。
- 若真实 UI 操作受系统权限限制，必须记录未执行项，以单测/代码审核/渲染证据替代但不得伪造。

## Architecture Gate

- [x] Task Center 与单视频 Workspace 所有权已分离
- [x] 文件夹扫描、配置快照、输出冲突和恢复语义已冻结
- [x] 单并发、取消、迟到事件与内存不变量已明确
- [x] Domain/Ports/Adapters/UI 依赖方向和停止条件已明确
- [x] 08102–08410 的顺序、验收与证据落点已拆解
