# Phase 8 批量任务中心设计合同

> 状态：Phase 8 已全部完成（08001 冻结合同 → 08102–08410 实现与综合验收收口，2026-08-13）。
>
> 视觉参考：[`08-task-center-future.png`](assets/08-task-center-future.png)。参考图只定义信息层级，
> 不授权展示 Whisper、虚假 ETA、平均置信度或其他尚无真实数据的能力。

## 1. 目标与用户路径

Phase 8 在现有单视频 Native Workbench 之外增加独立的 **Task Center Window**，服务于
“选择一批本地视频并可靠地产出 SRT”的长任务。用户可以：

1. 通过原生选择器或拖拽添加多个文件、一个或多个文件夹；
2. 预览被接受、跳过和拒绝的输入及原因；
3. 在任务进入执行前确认引擎、采样等级、输出位置和冲突策略；
4. 启动串行队列，观察每项真实阶段与进度；
5. 请求“完成当前任务后暂停”、立即取消当前任务、恢复队列或重试失败项；
6. 在完成后定位源文件或 SRT，并从本地持久化清单恢复历史任务。

Task Center 不替代 Main Workspace。Workspace 继续拥有单视频预览、交互式字幕区域、
处理期只读 Transcript、完成后校对编辑和手动导出；Task Center 只做无需逐文件交互的批量提取。

## 2. 输入合同

### 2.1 添加入口

- Toolbar 提供“添加文件”和“添加文件夹”两个明确入口；窗口空态和内容区均接受拖拽。
- 文件选择器允许一次选择多个 MP4、MOV、MKV。
- 文件夹选择器允许一次选择多个目录；“包含子文件夹”默认关闭，由用户显式开启。
- 文件选择、文件夹选择和拖拽必须进入同一个 `BatchInputScanner`，不得在 View 中复制扫描规则。

### 2.2 扫描规则

- 扩展名匹配不区分大小写，只接受 `VideoImportPolicy` 声明的 MP4/MOV/MKV。
- 默认只扫描文件夹直接子项；递归开启后按稳定相对路径顺序遍历。
- 不进入隐藏目录、macOS package 内容或符号链接；符号链接既不跟随也不递归。
- 使用标准化文件 URL 去重；已经存在于队列的源文件不重复加入。
- MKV 在 ffmpeg/ffprobe 不可用时 fail-closed，并显示可操作原因。
- 空目录、无支持视频、无读取权限、重复和不支持格式均进入扫描摘要，不静默丢弃。

扫描只产生候选与拒绝报告，不启动 metadata、Worker 或 OCR。

## 3. 任务与队列状态

### 3.1 Task 状态

```text
waiting → preparing → extracting → exporting → completed
             │             │           │
             ├─────────────┴───────────┴→ failed
             ├──────────────────────────→ cancelled
             └── application exit ──────→ interrupted

failed / cancelled / interrupted → retry → waiting
waiting → output collision with skip policy → skipped
```

- `waiting`：可重排、删除或显式批量修改配置。
- `preparing`：配置与输出计划锁定，正在做 metadata/runtime/output 前置检查。
- `extracting`：唯一活动 Worker 正在发送真实 progress。
- `exporting`：最终 entries 已到达，正在安全写出 SRT。
- `completed`：SRT 原子落地后才可进入；记录条目数和输出路径。
- `failed`：当前任务失败；不会阻断后续任务。
- `cancelled`：用户取消当前任务或等待任务。
- `interrupted`：应用退出时处于 preparing/extracting/exporting 的任务；重启后不自动执行。
- `skipped`：按“已有文件则跳过”等显式策略未执行的任务。

迟到的 progress、entries 或 completion 必须同时通过 task ID 和 run token 校验，不能污染已结束
任务或下一任务。

### 3.2 Queue 状态与控制

```text
idle → running → pauseRequested → paused → running
          │              │
          └── stop/cancel┴→ idle 或 paused
```

- Phase 8 固定最大并发数为 1；“队列”不等于并行 OCR。
- “暂停”语义是**完成当前任务后暂停**，不挂起 C++/Python Worker 进程。
- “取消当前任务”走现有快速取消路径；资源完全回收后才能启动下一项。
- “停止队列”取消当前任务并停止继续调度；等待项保持原顺序。
- 一个任务失败后默认继续下一项；错误保留在失败任务中供重试。

## 4. 配置快照

- 新任务默认复制 Settings/快速设置栏当前可见的 `ExtractionConfiguration`。
- 非开发模式下隐藏 Mock 必须在入队和启动前两次归一化，不能运行陈旧 Mock 偏好。
- waiting 任务允许用户显式批量修改引擎/采样等级；进入 preparing 后锁定。
- Settings 在任务等待或运行中变化，不追溯修改已有任务。
- 任务记录实际 runtime identity；界面不把选择的 engine 冒充为实际 runtime。
- Phase 8 批量任务固定使用 `region_box=nil` 的 Worker 默认底部区域；逐文件候选区和交互式
  Region Editing 只在 Main Workspace 提供。

## 5. 输出合同

### 5.1 输出位置

- 默认使用源视频同目录、同 basename 的 `.srt` sidecar。
- 用户可选择公共输出目录。对于文件夹输入，公共输出目录保留相对目录结构；独立文件直接使用 basename。
- 输出计划在执行前可预览；不可写目录在 preparing 阶段 fail-closed。

### 5.2 冲突策略

提供三种显式策略：

1. **跳过已有文件（默认）**：不启动 OCR，任务进入 skipped；
2. **自动重命名**：生成 `name (2).srt`、`name (3).srt` 等可用路径；
3. **替换已有文件**：开始队列前集中确认，不允许隐式覆盖。

公共输出目录内来自不同来源但目标相同的任务也必须走同一冲突规划器，不能在执行时互相覆盖。

### 5.3 写入安全

- 最终 entries 先由现有 `SrtFormatter` 格式化。
- 在目标目录写同卷临时文件，成功后原子移动/替换。
- 写入、同步或替换失败时任务为 failed；既有目标保持不变，临时文件清理。
- completed 任务只持久化摘要、字幕数量与输出路径，不长期保存完整 entries，保证长队列内存有界。

## 6. Task Center 信息架构

```text
Task Center Window
┌──────────────────────────────────────────────────────────────────────┐
│ Toolbar: Add Files / Add Folder / Start / Pause / Cancel / More     │
│ Search                                                Status Filter │
├──────────────────────────────────────────────────────────────────────┤
│ Table: File | Duration | Engine | Status | Progress | Output | Added│
├──────────────────────────────────────────────────────────────────────┤
│ Selected Task Detail: source / config / runtime / error / output    │
├──────────────────────────────────────────────────────────────────────┤
│ Summary: total · running · waiting · completed · failed             │
└──────────────────────────────────────────────────────────────────────┘
```

- 独立 Window，不在 Main Workspace 增加永久 Sidebar。
- Table/List 是任务主视图；Detail 只显示当前选择，不复制编辑表单。
- 搜索和筛选只改变投影，不改变队列真实顺序。
- 运行任务不能重排或修改配置；waiting 任务可重排、删除和批量修改。
- 状态使用图标、文字和系统语义色共同表达，不只依赖颜色。
- 只显示真实的 progress、frame count、runtime、字幕数量和输出路径；无真实协议数据时不显示 ETA。

## 7. 持久化与恢复

- 清单位置：Application Support 下的 SubLift 私有目录，文件名 `batch-queue-v1.json`。
- 使用带 `schemaVersion` 的 Codable JSON；保存采用临时文件 + 原子替换。
- 保存任务摘要、顺序、配置、输出计划、状态、计数、错误和时间；不保存视频内容、帧、字幕全文或日志全文。
- 应用启动时恢复队列，但任何准备中/提取中/导出中任务归一化为 interrupted。
- 恢复后默认 paused，由用户显式继续或重试，不在启动时拉起 Worker。
- JSON 损坏时 fail-closed：保留损坏文件用于诊断，展示“无法恢复任务清单”，允许用户创建新清单。
- Phase 8 开发者构建不实现 App Sandbox security-scoped bookmark；未来分发阶段单独设计。

## 8. 响应式、键盘与辅助功能

- 目标窗口默认 1280×800，最低 960×600；紧凑宽度隐藏低优先级列并保留文件、状态、进度和主控制。
- 长文件名与深层路径截断但提供完整 accessibility value / tooltip。
- Toolbar/Menu 复用统一命令可用性；破坏性操作需要确认。
- 键盘至少覆盖添加文件、添加文件夹、开始/暂停、取消、删除等待任务、搜索和在 Finder 显示。
- 每行有“文件名、状态、进度、引擎、输出”的可读组合标签；进度变化不过度打断 VoiceOver。
- Light/Dark、Accent、Increase Contrast、Reduce Transparency 和 Reduce Motion 遵循 Phase 10 系统语义。

## 9. 明确非目标

- 并行运行多个 OCR Worker 或可配置并发数；
- 文件夹自动监听、后台 daemon、计划任务或网络同步；
- Whisper、Automatic engine、新 OCR Provider 或引擎对照；
- ASS/VTT、翻译、软字幕抽取或视频写回；
- 每任务交互式字幕区域、批量字幕编辑或完整 Workspace 会话持久化；
- 修改 C++/Python Pipeline、OCR 算法、UDS framing、默认 runtime 或 fail-closed 策略；
- `.app` 分发、App Sandbox、security-scoped bookmark、签名或公证。

## 10. 视觉验收矩阵

| ID | 状态 | 必须可见 |
|---|---|---|
| B01 | Empty | 单一“添加文件”主动作、“添加文件夹”次动作、本机处理说明 |
| B02 | Scanned | 接受/跳过/拒绝摘要，原因可展开，不自动启动 |
| B03 | Waiting | 任务顺序、冻结配置、输出计划和开始动作 |
| B04 | Running | 唯一活动项、真实阶段/进度、完成当前项后暂停、取消当前任务 |
| B05 | Paused | 无活动 Worker、等待项保留、恢复主动作 |
| B06 | Mixed Result | completed/failed/skipped 同时存在，失败不阻断后续项 |
| B07 | Restored | interrupted 任务、暂停队列、显式重试/继续 |
| B08 | Compact | 960×600 下无裁切/溢出，主控制与状态仍可达 |
| B09 | Dark | Dark + 非默认 Accent 下状态不只依赖颜色 |
| B10 | Error | 不可写、Worker/capability、清单损坏均有可操作错误 |

