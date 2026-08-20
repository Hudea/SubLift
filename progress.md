# SubLift 进度导航

## 当前状态

- **最后更新：** 2026-08-20
- **当前 Phase：** [Phase 13 - 退役 Python Runtime，建立 Native-only 产品](docs/phases/phase13.json)（`in-progress`）
- **当前 Deliverable：** D04 Python-free 产品门与测试迁移
- **进度真源：** [`phases.json`](phases.json) → `detail_file`；本文件只提供当前工作与阻塞导航。

## Phase 13 目标

在无 Python、uv、`.venv` 的环境中，使 Native CLI、macOS 与 Web 均可完成模型与 ORT
资源准备、视频提取、SRT 导出和产品验证。产品不再提供 Python Runtime 回退；需要回滚时
回到上一可用产品版本。Benchmark、评分、parity 与冻结 Oracle 仅作为隔离的可选离线工具。

当前顺序：

1. D01 退役决策与验收基线 — `done`
2. D02 关闭产品双 Runtime — `done`
3. D03 Native 模型与 ORT 资源闭环 — `done`
4. D04 Python-free 产品门与测试迁移 — `not-started`
5. D05 移除活跃 Python 产品实现 — `not-started`
6. D06 隔离可选离线工具并最终收口 — `not-started`

执行 Task 不写入 Phase 文件；当前会话只推进一个 Deliverable。全部 Deliverable 完成后，
Phase 先进入 `ready-for-merge`；合入 `main` 并在 `main` 上完成最终验证后才标记 `done`。

## 阻塞项

- [Phase 12](docs/phases/phase12.json) / 12505 — `blocked`：本地修复与降级验收已有证据，
  但尚无真实 Docker build/run、容器内 `paddle.available=true` 和 golden SRT 比对证明；
  取得这些外部环境证据前不得将 Phase 12 标记为完成。

## 最近收口

- Phase 12 除 12505 容器实证外的 Native Server、Web Workbench、Task Center、媒体工作区与本地验证能力已交付。
- Phase 10 macOS Native Workbench UI 已完成。
- Phase 8 macOS Batch Task Center 已完成。
