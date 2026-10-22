# Web / macOS Parity Matrix

> 状态：Phase 17 D01/D02。`shared` = 必须一致；`host` = 允许按宿主不同；`gap` = 当前不一致、
> 由 D02/D03 收敛或显式接受；`deferred` = 本 Phase 不做。新能力或行为变更必须先在此表登记。

## 1. 合同边界

两端共享的是**能力语义**，不是 UI 代码或像素：

- 同一输入（视频、配置、ROI）应得到相同字幕语义；SRT 字节、输出重名、状态与错误映射一致。
- macOS 的文件面板、Finder、本地 sidecar、UDS；Web 的媒体根、服务端扫描、HTTP/SSE、token
  属于宿主差异，但必须通过同一合同暴露必要信息。
- Web 不因为“在浏览器里”而拥有更弱的任务/队列语义；macOS 不因为“本地”而绕过 capability
  fail-closed。

## 2. 矩阵

| 能力 | macOS / Swift | Web | 合同 | 状态 |
|---|---|---|---|---|
| 单视频 Session 状态 | 10 态（empty/loading/ready/regionEditing/starting/processing/finalizing/review/failed/cancelled） | 6 态（Empty/Ready/Processing/Review/Failed/Cancelled） | `job-snapshot` + parity 映射 | gap |
| Job/Task 状态 | Task 9 态；Worker progress/finalizing | Task 9 态；Server queued/running | `statuses.json` | gap |
| 提取配置 | engine + quality（无 confidence/script UI） | engine + quality；batch 还有 ROI policy | `extraction.schema.json` | gap |
| 默认值（confidence） | 0.0 | 0.0 | `defaults.json`（规范 0.0） | shared |
| 默认值（engine/script） | engine 由 Worker/capability 选择；script auto | engine 由 capability 选择；script auto | `defaults.json` | 待 D02 核 |
| ROI 坐标 | Worker 像素 `[x,y,w,h]` | HTTP/UI 归一化 box | `roi-cases.json` | shared |
| SRT 字节 | 排序、跳过空白、钳制；与 fixture 一致 | 排序、跳过空白、钳制；与 fixture 一致 | `srt-cases.json` | shared |
| 输出重名 | `name_1.srt` | `name_1.srt` | `output-cases.json` | shared |
| 批量队列持久化 | versioned JSON Repository | 无（仅单视频草稿 localStorage） | 待 D03 | gap |
| 批量 ROI | 固定底边 | auto/fixed/default | 待 D02 决策 | gap |
| 字幕编辑 | 拆分/合并 | 合并/插入/删除/撤销/草稿，缺拆分 | 待 D02/D05 | gap |
| Capability 展示 | Worker probe（vision/paddle/mock） | Server `/api/system/info` | 共享字段待 D02 | gap |
| 媒体根 | 单个视频文件 URL | Server media root + 路径沙箱 | host | host |
| 文件选择/扫描 | NSOpenPanel / 本地扫描 | 浏览器文件夹 / 服务端 scan-path | host | host |
| 鉴权 | 无 | 可选 Bearer；EventSource 不带 header | 待 D02/D03 | gap |
| UI 设计基线 | `docs/design_ui/` 已有 | 无独立设计基线 | D05 | gap |
| ASS/VTT、GPU、账号、公网 | — | — | non-goals | deferred |

## 3. 收敛顺序

1. **D02 已冻结 SRT、输出重名、confidence 默认值与 ROI 转换**：C++/Swift/Web 的 fixture 与请求默认值消费通过；下一步 Job/Task 状态映射与 engine/script 核对。
2. **D02 再冻结状态**：Job/Task 映射、stage 名称、取消/中断语义。
3. **D03 补 Web 队列持久化**：与 Swift `BatchQueueState` 语义对齐，优先版本化 JSON。
4. **D05 处理表现层差异**：编辑能力（split/undo/draft）与 UI 设计基线，按产品价值取舍，
   不能继续以“实现顺手”为理由分叉。

## 4. 评审规则

- 任何跨端行为 PR 必须说明：是否改变 shared 行、是否需要新 fixture、是否更新本表。
- `gap` 不是永久状态；D02/D03/D05 完成时改为 `shared` 或 `host`，并在 Phase 17 evidence
  中记录。
- 不允许把 `host` 当作偷懒标签：只有平台 API、文件系统所有权或部署边界造成的差异才可标
  `host`。
