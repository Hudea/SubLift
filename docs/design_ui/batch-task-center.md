# Task Center 设计

## 1. 产品边界

Task Center 服务于“导入一批本地视频并可靠地产出 SRT”。它使用独立的 `BatchQueueModel`，
不替代单视频 Workbench，也不共享可变 `SubtitleExtractor`。

```text
SubLiftMacApp
├── WorkspaceModel：单视频预览、选区、提取、校对、手动导出
└── BatchQueueModel：多文件/文件夹、串行队列、自动输出、恢复
```

## 2. 输入

- Toolbar 提供“添加文件”和“添加文件夹”，由单一 importer 协调。
- 空队列显示可拖入的空画布，不渲染空 Table 或第二套按钮。
- 文件选择、文件夹选择和拖拽统一进入 `BatchInputScanner`。
- 接受 MP4、MOV、MKV；扩展名不区分大小写。
- 默认不递归；不进入隐藏目录、package 或符号链接。
- 标准化 URL 后去重，并分别报告接受、跳过和拒绝原因。
- 目录输入记录 `importRootURL`，零散文件为 `nil`，用于位置展示和公共输出根规划。

## 3. 任务与队列

```text
waiting → preparing → extracting → exporting → completed
                     └──────────────→ failed / cancelled / interrupted
waiting ────────────────────────────→ skipped / cancelled / interrupted
```

队列状态为 `idle / running / paused / interrupted`，固定单并发。请求暂停表示完成当前任务后停止调度，
不 suspend Worker。暂停原因区分三种：完成后暂停（运行中可再次点击撤销）、单独运行某任务、
取消当前任务（后两者的暂停不可撤销，防止单任务契约被软化或已取消的队列被误点复活）。
失败任务不阻断后续任务；Retry 创建新的运行 token 并清除上一轮失败、进度和结果。

## 4. 配置快照

- 任务在入队时复制引擎与采样配置。
- `waiting` 可显式修改；进入 `preparing` 后锁定。
- Settings 后续变化不追溯已有任务。
- 批量任务默认使用 Worker 的底部字幕区域；逐视频 Region Editing 属于 Workbench。

## 5. 输出

- 默认输出到视频旁的同名 sidecar SRT。
- 队列可选择公共输出目录；目录导入保留相对结构。
- 导入完成后即计算输出预览，但不创建 SRT、不启动 Worker。
- Table 与 Inspector 在开始前显示目标路径；已存在目标显示明确 warning。
- 开始时统一处理目标已存在、不可写、越界和同批碰撞；取消确认不触碰文件。
- 最终内容通过同卷临时文件原子落地，成功后再标记 `completed`。

## 6. 信息架构

- Toolbar：添加文件、添加文件夹、输出位置、开始队列/完成后暂停（运行中再次点击可撤销）/继续队列 + 取消当前（仅活动任务时）。
- 汇总条展示队列状态文案（运行中/完成后将暂停/正在取消当前任务/已暂停），并纳入无障碍标签。
- Table：文件、位置、状态、进度、输出和操作；窄窗口可把位置折入文件单元格。
- 搜索匹配文件名、路径和导入位置；筛选结果为空不等于队列空态。
- 单选任务显示一张原生 Inspector 卡片，包含位置、输出、配置和安全操作，不展示内部 Task ID。
- Waiting 任务可删除、重排和修改配置；活动任务锁定影响运行语义的控件。
- Finder 定位只在对应源或输出实际存在时启用。

## 7. 持久化与恢复

- 队列使用 versioned Codable JSON 原子保存，不引入数据库。
- 保存任务、配置、输出目标、队列状态和必要结果摘要；不持久化运行 token。
- 暂停请求是运行时状态，不进 JSON；draining 中退出按中断恢复，暂停意图需用户重新发起。
- 恢复时活动任务转为 `interrupted`，队列转为 `paused`。
- 应用启动后不自动恢复执行，用户必须显式继续。
- 损坏或未知版本 fail-closed，并提供可操作错误。

## 8. 安全与可访问性

- 单并发、run token 和迟到回调过滤共同防止任务串扰。
- 完成后不长期持有完整字幕或视频帧，只保留必要摘要和输出位置。
- 路径计算使用组件级相对路径，不使用字符串替换推导目录关系。
- 状态使用图标、文字和颜色共同表达；Toolbar、Table、Inspector 与队列操作支持键盘和辅助功能标签。

参考设计与实际截图分别见 [参考资产索引](reference-index.md)和
[`evidence/08308`](evidence/08308/)、[`evidence/08309`](evidence/08309/)、
[`evidence/08511`](evidence/08511/)。

## 9. 非目标

- 不实现并行 OCR、目录监听、Whisper、ASS/VTT 或逐任务自定义输出路径。
- 不把 Task Center 合并进 WorkspaceModel。
- 不在 UI 展示无法稳定提供的 ETA、平均置信度或虚假引擎能力。
- 不改变 Worker/IPC、OCR 算法或默认 runtime。
