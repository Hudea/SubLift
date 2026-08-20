# SubLift 进度导航

## 当前状态

- **最后更新：** 2026-08-20
- **当前 Phase：** [Phase 13 - 退役 Python Runtime，建立 Native-only 产品](docs/phases/phase13.json)（`ready-for-merge`）
- **当前 Deliverable：** 无（D01–D06 均 `done`）。合入 `main` 并在 `main` 上复验产品门后才可将 Phase 置 `done`。
- **进度真源：** [`phases.json`](phases.json) → `detail_file`；本文件只提供当前工作与阻塞导航。

## Phase 13 目标

在无 Python、uv、`.venv` 的环境中，使 Native CLI、macOS 与 Web 均可完成模型与 ORT
资源准备、视频提取、SRT 导出和产品验证。产品不再提供 Python Runtime 回退；需要回滚时
回到上一可用产品版本。Benchmark、评分、parity 与冻结 Oracle 仅作为隔离的可选离线工具。

产品验证：`./scripts/verify-product.sh`。离线工具：`./scripts/verify-offline.sh`。

## 阻塞项

- [Phase 12](docs/phases/phase12.json) / 12505 — `blocked`：本地修复与降级验收已有证据，
  但尚无真实 Docker build/run、容器内 `paddle.available=true` 和 golden SRT 比对证明；
  取得这些外部环境证据前不得将 Phase 12 标记为完成。

## 最近收口

- Phase 13 D01–D06 已在当前分支完成，Phase 进入 `ready-for-merge`（尚未合入 main）。
- Phase 12 除 12505 容器实证外的 Native Server、Web Workbench、Task Center、媒体工作区与本地验证能力已交付。
- Phase 10 macOS Native Workbench UI 已完成。
- Phase 8 macOS Batch Task Center 已完成。
