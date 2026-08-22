# SubLift 进度导航

## 当前状态

- **最后更新：** 2026-08-22
- **当前 Phase：** 无（[Phase 13](docs/phases/phase13.json) 已在 `main` 完成验收并置 `done`）
- **当前 Deliverable：** 无。所有已登记 Deliverable 均已交付。
- **进度真源：** [`phases.json`](phases.json) → `detail_file`；本文件只提供当前工作与阻塞导航。

## Phase 13 目标（已达成）

在无 Python、uv、`.venv` 的环境中，使 Native CLI、macOS 与 Web 均可完成模型与 ORT
资源准备、视频提取、SRT 导出和产品验证。产品不再提供 Python Runtime 回退；需要回滚时
回到上一可用产品版本。Benchmark、评分、parity 与冻结 Oracle 仅作为隔离的可选离线工具。

产品验证：`./scripts/verify-product.sh`。离线工具：`./scripts/verify-offline.sh`。

## 阻塞项

- [Phase 12](docs/phases/phase12.json) / 12505 — `blocked`：Linux 容器交付暂缓，活跃 Docker
  资产与容器验收分支已移除；本地 Native Server 与 Web 工作台不受影响。未来恢复该范围时
  需重新实现容器分发并取得真实运行、Paddle capability 与 golden SRT 比对证据。

## 最近收口

- Phase 13 已合入 `main`，通过 `./scripts/verify-product.sh` 并在 `main` 上冻结为 `done`。
- Phase 12 除 12505 容器实证外的 Native Server、Web Workbench、Task Center、媒体工作区与本地验证能力已交付。
- Phase 10 macOS Native Workbench UI 已完成。
- Phase 8 macOS Batch Task Center 已完成。
