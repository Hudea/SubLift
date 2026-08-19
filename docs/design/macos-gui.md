# macOS GUI 模块设计

> `apps/macos/Sources/SubLiftMac/` — SwiftUI 壳、Native Worker 编排、UDS 客户端、预览、编辑与导出。
>
> ADR-0005 记录了最初的 SwiftUI + Python Worker 方案，仅用于历史追溯。Phase 13 的产品
> 合同是 Native-only：GUI 只认识并启动 C++ `sublift_worker`，不提供运行时实现切换；
> 产品回滚通过回滚到上一已验收版本完成。
>
> **当前实现：** Phase 10 Native Workbench 已实施；视觉、交互状态和验收边界见
> [`docs/design_ui/`](../design_ui/README.md)，逐 Feature 证据见
> [`docs/phases/phase10.json`](../phases/phase10.json)。
>
> **已实现：** Phase 8 完成独立 Task Center（⌘⇧T）、多文件/文件夹统一导入（同一 Scanner）、单并发串行队列、安全 SRT 输出（skip/rename/replace）、本地 JSON 恢复与批量交互（筛选/多选/重排/显式改配置）。
> SRT 与本地恢复；合同见 [`batch-task-center.md`](../design_ui/batch-task-center.md)，跟踪见
> [`phase8.json`](../phases/phase8.json)。

## 运行时边界

GUI 不在 Swift 进程中执行字幕 Pipeline。Swift 只选择 engine、探测 Native capability，
并由 `PipelineClient` 启动 C++ Worker；不存在第二套产品 runtime。

| 选择 | 后端 | 行为 |
|---|---|---|
| vision / mock | C++ `sublift_worker` | 产品路径 |
| paddle，capability 与模型可用 | C++ `sublift_worker` | 产品路径 |
| Worker、capability 或模型不可用 | 无 | fail-closed，显示可操作错误 |

不允许因 C++ capability 缺失而静默切换其他实现、Vision 或 Mock。GUI 记录实际 Worker
与 engine capability，避免界面设置与真实执行后端漂移。

当前代码中 `RuntimePolicy`、`PipelineClient`、部分测试及仓库定位仍保留 Python 分支和
`.venv` 假设。这些是 Phase 13 待移除的迁移债务，不是兼容承诺；新增 GUI 行为不得继续
扩展该路径。

## 模块职责

### Core

| 文件 | 职责 |
|---|---|
| `WorkspaceModel.swift` / `WorkspaceState.swift` | 单窗口 Session 组合根；校验状态转换、job token、Inspector intent、Transcript 权限和 command availability。 |
| `WorkspaceLayout.swift` | 计算 Video/Transcript/Inspector 的比例、最小宽度与 divider clamp。 |
| `VideoImportPolicy.swift` | 统一 Open/drop 格式校验；MKV 缺 ffmpeg 时在改变 Session 前 fail-closed。 |
| `ExtractionConfiguration.swift` | 表达引擎与采样质量的 active/final 运行配置快照。 |
| `RuntimePolicy.swift` | 过渡模块；目标只解析 engine/capability 并执行 Native fail-closed，移除 runtime 切换。 |
| `PipelineClient.swift` | 查找并启动 C++ Worker；负责 UDS 连接、framing、握手、请求与资源回收。现存 Python 分支待删除。 |
| `Messages.swift` | Swift ↔ C++ Worker 的 IPC Codable 消息。 |
| `SubtitleExtractor.swift` | 发送 path-mode `start_job`，消费 progress / push_entry / entries / done，协调取消、日志与处理倍速。 |
| `FrameSampler.swift` | 为预览候选框扫描、代表帧和 legacy frame mode 提供 AVFoundation 采样；不参与默认打轴。 |
| `FfmpegFallback.swift` | 为 mkv 预览/代表帧、元数据及 legacy frame mode 提供系统 ffmpeg 兜底。 |
| `VideoMetadata.swift` | 异步读取文件名、大小、分辨率、时长和编码；mkv 使用 ffprobe 兜底。 |
| `VisionTextDetector.swift` | 在 GUI 内对代表帧检测文字候选框；只服务选区，不承担产品 OCR。 |
| `RegionSelectionModel.swift` | 管理候选框多选，并推算全宽字幕带区域。 |
| `VideoCoordinateMapper.swift` | 转换像素、视图与 Vision 归一化坐标。 |
| `SubtitleEditor.swift` / `SubtitleEntry.swift` | 维护可编辑字幕、当前高亮、合并与拆分。 |
| `SrtFormatter.swift` | 在 Swift 端格式化 SRT，不调用 Worker。 |
| `ProcessingRate.swift` / `PreviewLayout.swift` | 处理倍速格式化与预览布局纯计算。 |

### UI

| 文件 | 职责 |
|---|---|
| `SubLiftMacApp.swift` / `WorkspaceCommands.swift` | `@main`、WorkspaceModel 生命周期、系统菜单与快捷键。 |
| `Welcome/WelcomeView.swift` / `DropZone.swift` | 空工作区、Open 主动作与统一视频拖入目标。 |
| `Workspace/WorkspaceRootView.swift` / `WorkspaceSplitView.swift` | Native 根视图、Toolbar、Video/Transcript Split、Inspector 和提取请求入口。 |
| `VideoPreview.swift` | AVPlayer 预览、播放/暂停与 seek。 |
| `RegionOverlay.swift` / `Region/RegionInspector.swift` | Region Editing 候选、多选、合并区域与几何投影。 |
| `Transcript/TranscriptPanel.swift` / `TranscriptRow.swift` | 处理期只读与 Review 编辑、搜索、selection/current 和上下文命令。 |
| `Workspace/WorkspaceInspector.swift` / 各 Inspector | Video / Region / Extraction / Subtitle 上下文详情；Extraction 配置只读。 |
| `Extraction/ExtractionProgressView.swift` / `QuickExtractionSettingsBar.swift` | 真实进度、Worker 状态、Stop，以及共用 AppStorage 的紧凑引擎/采样设置和重新提取确认。 |
| `Timeline/SubtitleTimelineView.swift` | 字幕条带、播放头、点击 seek 与前后字幕导航。 |
| `Settings/SettingsView.swift` / `EngineCapability.swift` | General / Recognition / Advanced 分区、真实 capability 与 Developer Mode。 |
| `TimeFormatter.swift` | 毫秒时间格式化。 |

## IPC 协议

### 传输与进程

- **传输**：Unix Domain Socket；Swift 为每次连接生成临时 socket 路径。
- **分帧**：4 字节大端 body 长度 + UTF-8 JSON。
- **C++ 启动**：`sublift_worker --socket <path> --engine <engine>`。
- **关闭**：`PipelineClient.stop()` 幂等关闭 fd、终止子进程并 unlink socket。
- **能力探测**：Paddle 路由前对将要启动的 C++ Worker 执行 `--probe-engine paddle`。

### 消息类型

| 方向 | 消息 | 关键语义 |
|---|---|---|
| Swift → Worker | `hello` / `bye` | 能力握手与连接关闭。 |
| Swift → Worker | `start_job` | `video_id`、`video_path?`、fps、engine、region、subtitle profile；有 `video_path` 即 path mode。 |
| Swift → Worker | `cancel_job` | 取消当前作业并释放 extractor / Pipeline 资源。 |
| Swift → Worker | `frame` / `finalize` | 仅 legacy frame mode；产品 GUI 不使用。 |
| Worker → Swift | `progress` | ready / processing / finalizing 阶段与 `pct`。 |
| Worker → Swift | `push_entry` | 字幕段闭合并完成 OCR 后的增量条目。 |
| Worker → Swift | `entries` | 最终 dedupe 后的全量结果，`is_final=true`。 |
| Worker → Swift | `log` / `done` / `error` | 诊断、业务结束与协议错误。 |

Worker 启动时绑定的 engine 是实际执行身份；若与 `start_job.engine` 不一致，返回明确失败，
不得以其他引擎继续。单条消息上限为 64 MiB；Swift 与 C++ Worker 共同遵守 framing
边界和字段契约。

## 默认 GUI 数据流

```text
用户 Open 或拖入视频
  ↓
VideoImportPolicy 校验 → WorkspaceModel 创建/替换 Session
  ↓
VideoMetadata + AVPlayer 预览 → VisionTextDetector 候选 → Region Editing
  ↓
WorkspaceRootView.requestExtraction 冻结 active ExtractionConfiguration
  ↓
Native capability resolve(engine)
  ↓
PipelineClient 启动 C++ Worker 并握手
  ↓
start_job(video_path, region_box, subtitle_profile)
  ↓
Worker-owned FfmpegExtractor → Pipeline.feed / ocr_segment / finalize
  ↓
progress + push_entry → WorkspaceModel 的只读 Live Transcript 投影
  ↓
entries(is_final=true) → SubtitleEditor 原子载入 + final 配置快照 → Review
  ↓
TranscriptPanel 编辑/搜索/定位 → SrtFormatter → NSSavePanel 写文件
```

默认 path mode 不发送 JPEG `frame`，避免 AVFoundation 跳采样相位与有损编码造成质量漂移。
Native extractor 的时间戳、ROI、像素和取消行为由 CTest 与冻结 parity/golden 契约约束。

## Phase 8 批量数据流（已实现）

```text
Task Center 添加文件/文件夹
  ↓
BatchInputScanner → 接受/跳过/拒绝摘要 → BatchTask waiting
  ↓ 用户开始
BatchQueueScheduler（maxActive=1）
  ↓
BatchExtractionRunner → 每任务独占 PipelineClient/Worker
  ↓ progress/final entries
SrtFormatter → AtomicSrtWriter → completed 摘要
  ↓
BatchQueueRepository → versioned JSON；重启 active → interrupted/paused
```

该计划不修改上面的单视频 Workspace 数据流。批量 Runner 使用 `region_box=nil` 的 Worker 默认
底部区域，不共享 Workspace 的 Region/Editor/Extractor；Task Center 和 Workspace 只共享
`ExtractionConfiguration`、`VideoImportPolicy`、`SrtFormatter` 等值类型或无状态能力。

## 预览、选区与 legacy frame mode

- AVFoundation 负责播放预览和常见容器代表帧。
- 系统 ffmpeg 负责 mkv 预览/元数据兜底；缺失时 UI 提示安装。
- `VisionTextDetector` 只决定 `region_box`，不替代 Worker 内 OCR。
- 无 `video_path` 时协议仍支持 Swift 推 JPEG 的 legacy frame mode，供兼容测试和诊断；
  产品 GUI 始终发送视频路径并使用 Worker-owned path mode。
- legacy frame mode 继续限制 JPEG 大小、解码后像素数并拒绝无效 Base64/JPEG。

## 字幕区域与坐标

`RegionSelectionModel` 默认选择画面下部文字候选。多个候选合并为 source-frame 坐标：

```text
x = 0
width = video_width
y = min(selected.y) - padding
height = max(selected.maxY) - min(selected.y) + 2 * padding
```

`region_box` 随 `start_job` 一次发送。有效固定区域进入 ROI path；未选择区域时 Worker
使用 BottomCrop 语义。GUI 视图坐标、Vision 归一化坐标与 source-frame 像素坐标只在
`VideoCoordinateMapper` 中转换。

## 字幕编辑与导出

`SubtitleEditor` 在主线程维护带 UUID 的最终可编辑条目；WorkspaceModel 在
starting/processing/finalizing 阶段只暴露只读 Transcript，最终 entries 原子落地进入 Review
后才开放以下 mutation：

| 操作 | 行为 |
|---|---|
| load | 以 Worker 最终 entries 替换增量列表。 |
| updateText | 修改单条文本。 |
| merge | 与下一条合并时间范围和文本。 |
| split | 在时间中点拆分，后段文本置空。 |
| updateCurrent | 根据播放时间节流更新当前条目。 |

编辑后的导出完全在 Swift 端完成。`SrtFormatter` 校验非负时间和 `end >= start`，输出
`HH:MM:SS,mmm`、条目间空行及末尾换行。

## 已知约束

- 当前只支持 SwiftPM 开发者运行；独立 `.app`、依赖随包、签名和公证后置。
- `RuntimePolicy`、`PipelineClient` 和 Swift 集成测试中残留的 Python 路由、`.venv` 根目录
  假设尚待 Phase 13 清除；在此之前不得把它们描述为产品回退能力。
- legacy frame mode 仍是协议兼容面，移除前必须先审计测试与外部消费者。
- 批量队列已完成 Phase 8（08001 规划 + 08102–08410 实现与综合验收）；精细时间码拖动仍未规划。
- 长视频交互已有历史手工验收，但尚未形成持续运行的跨片源 GUI 回归套件。
- V10 系统辅助功能设置切换、完整 VoiceOver 会话及部分真实点击受当前系统权限限制；自动测试、代码审核和 EvidenceShot 渲染覆盖边界见 Phase 10 evidence。
