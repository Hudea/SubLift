# macOS UI 设计

本目录维护 SubLift macOS 产品当前的交互与视觉设计参考，不记录 Phase 计划、实施步骤或项目进度。

| 文档 | 职责 |
|---|---|
| [Workbench 设计](macos-workbench.md) | 单视频工作台的信息架构、Surface 与产品交互 |
| [交互状态规范](interaction-state-spec.md) | Workspace 状态机、命令、只读/编辑和响应式合同 |
| [Task Center 设计](batch-task-center.md) | 批量导入、串行队列、输出规划、恢复与任务中心 UI |
| [参考资产索引](reference-index.md) | 外部参考图的来源、适用边界与哈希 |

## 资产

- `assets/` 保存外部设计参考，只表达布局、层级和视觉方向，不证明产品能力。
- `evidence/` 保存实际 SwiftUI 截图和 fixture 证据，可用于开发设计回看；目录编号保留历史来源，但不承担当前进度跟踪。

## 权威边界

需求与运行时边界以 [`REQUIREMENTS.md`](../REQUIREMENTS.md) 和
[`ARCHITECTURE.md`](../ARCHITECTURE.md) 为准；当前实现以
`apps/macos/` 代码与测试为可执行依据。本目录描述应维持的产品体验，发现实现与设计漂移时应明确校对，不从参考图推导未实现能力。

Task Center 与单视频 Workbench 是两个独立窗口和状态根；二者复用提取配置与 Worker 能力，但不共享可变任务状态。
