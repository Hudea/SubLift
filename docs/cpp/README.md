# Native C++ 文档

本目录只维护当前 Native 产品仍依赖的架构与契约，不记录 Phase 计划、迁移步骤或验收报告。

| 文档 | 职责 |
|---|---|
| [Native 架构](native-architecture.md) | CMake targets、依赖方向、Ports/Adapters 与宿主边界 |
| [Runtime 契约](runtime-contract.md) | 产品入口、引擎 capability、fail-closed 与显式 Python 路径 |
| [Worker IPC 契约](worker-ipc-contract.md) | Swift/CLI 与 Worker 的 UDS framing、消息时序和生命周期 |
| [Parity 契约](parity-contract.md) | 冻结 Oracle、golden、比较层级与更新规则 |

项目级系统全景见 [`docs/ARCHITECTURE.md`](../ARCHITECTURE.md)。接口与构建行为的可执行真源是
`cpp/include/`、`cpp/src/`、CMake target 以及对应测试；实现与本文冲突时，应先以测试确认实际行为，再同步修正文档。

## 长期边界

- 默认产品运行时使用 Native C++，缺失 capability 时 fail-closed。
- Core、Pipeline 与 Application 不依赖 UI、传输协议或具体 OCR/媒体 Adapter。
- 第三方类型和平台 Framework 不得泄漏到公共领域接口。
- Python 只承担 Oracle、benchmark、显式回滚与开发验证。
- 产品 target 不依赖 test support、diagnostics 或在线 Python Oracle。
