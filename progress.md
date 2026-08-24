# SubLift 进度导航

## 当前状态

- **最后更新：** 2026-08-24
- **当前 Phase：** [Phase 12](docs/phases/phase12.json) — `in-progress`（legacy Feature 结构）
- **当前 Feature：** 12510 — `done`，Web 批量任务中心交互增强与受控文件夹扫描（下一项待收口：12511）
- **已冻结里程碑：** [Phase 13](docs/phases/phase13.json) 已在 `main` 完成验收并置 `done`，不因 Phase 12 收口而重开。
- **进度真源：** [`phases.json`](phases.json) → `detail_file`；本文件只提供当前工作与阻塞导航。

## 当前收口顺序

- 12509 已补登记并完成：智能字幕区域识别与选区平滑吸附。
- 当前只推进 12510；完成后按 `12511 → 12512 → 12513 → 12514 → 12515 → 12516`
  依次收口媒体信任边界、任务恢复、配置/质量一致性、真实导出、审阅草稿和 Web 可访问性。
- 产品验证使用 `./scripts/verify-product.sh`；可选离线工具使用 `./scripts/verify-offline.sh`。

## 阻塞项

- 12505 — `blocked`：Linux 容器交付暂缓，活跃 Docker 资产与容器验收分支已移除；未来
  恢复该范围时需重新实现容器分发并取得真实运行、Paddle capability 与 golden SRT 证据。
  该外部阻塞不妨碍当前 12510–12516 的本地 Web/Native Server 收口。

## 当前风险

- 12511 完成前，工作区配置尚未成为所有媒体路由的统一授权根。
- 12512 完成前，Web 队列和 Native job 仍以内存状态为主，刷新/重启与 SSE 重连不可靠。
- 12513 完成前，单视频自动 ROI 与批量默认 ROI 可能造成同一视频的结果漂移。

## 最近收口

- Phase 13 已合入 `main`，通过 `./scripts/verify-product.sh` 并在 `main` 上冻结为 `done`。
- Phase 12 的 Native Server、Web Workbench、媒体工作区与智能区域检测基座已交付；完成度缺口见 12510–12516。
- Phase 10 macOS Native Workbench UI 已完成。
- Phase 8 macOS Batch Task Center 已完成。
