# Native C++ 文档

本目录只维护当前 Native 产品仍依赖的架构与契约，不记录 Phase 计划、迁移步骤或验收报告。

> ADR-0038 已确认 C++ 是唯一产品运行时；Phase 13 正在退役 Python 产品路径。本文描述目标
> 契约，现存 Python CLI、Pipeline、IPC、runtime 路由和资源准备耦合仍是待完成迁移债务。

| 文档 | 职责 |
|---|---|
| [Native 架构](native-architecture.md) | CMake targets、依赖方向、Ports/Adapters 与宿主边界 |
| [Runtime 契约](runtime-contract.md) | Native-only 产品入口、引擎 capability、fail-closed、版本回滚与过渡债务 |
| [Worker IPC 契约](worker-ipc-contract.md) | Swift/CLI 与 Worker 的 UDS framing、消息时序和生命周期 |
| [Parity 契约](parity-contract.md) | golden、GT、Native tests、比较层级与更新规则 |

项目级系统全景见 [`docs/ARCHITECTURE.md`](../ARCHITECTURE.md)。接口与构建行为的可执行真源是
`cpp/include/`、`cpp/src/`、CMake target 以及对应测试；实现与本文冲突时，应先以测试确认实际行为，再同步修正文档。

## 长期边界

- 产品只使用 Native C++ runtime，缺失 capability 时 fail-closed。
- Core、Pipeline 与 Application 不依赖 UI、传输协议或具体 OCR/媒体 Adapter。
- 第三方类型和平台 Framework 不得泄漏到公共领域接口。
- 版本回滚使用此前已验收的 Native artifact、release/tag 或 Git revision，不切换 Python。
- Python 最多作为隔离、可选、离线工具；产品 target、Native 日常验证与资源交付均不依赖它。
- 产品行为真源是版本化 golden、固定 GT 与 Native tests；最终 Python 源只由 Git/tag 保存，
  不在工作树建立 archive。
