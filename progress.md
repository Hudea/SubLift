# SubLift 进度导航

## 当前状态

- **最后更新：** 2026-08-24
- **当前 Phase：** [Phase 12](docs/phases/phase12.json) — `blocked`（12509–12516 全部完成交付；12505 容器分发保持 blocked）
- **当前 Feature：** 无（12509–12516 全部完成）
- **已冻结里程碑：** [Phase 13](docs/phases/phase13.json) 已在 `main` 完成验收并置 `done`。
- **进度真源：** [`phases.json`](phases.json) → `detail_file`；本文件只提供当前工作与阻塞导航。

## 当前收口顺序

- Phase 12 本地 Web / Native Server 纵向收口项（12509、12510、12511、12512、12513、12514、12515、12516）全部交付并闭环。
- 产品验证使用 `./scripts/verify-product.sh`；可选离线工具使用 `./scripts/verify-offline.sh`。

## 阻塞项

- 12505 — `blocked`：Linux 容器交付暂缓，活跃 Docker 资产与容器验收分支已移除；未来
  恢复该范围时需重新实现容器分发并取得真实运行、Paddle capability 与 golden SRT 证据。

## 当前风险

- 无未决风险。12511–12516 已解决媒体沙箱统一根、状态版本化持久化、配置/ROI 策略一致性、受控原子导出、审阅草稿恢复与可访问性。

## 最近收口

- Phase 13 已合入 `main`，通过 `./scripts/verify-product.sh` 并在 `main` 上冻结为 `done`。
- Phase 12 Native Server 与 Web 本地工作台已完成完整度收口（12509–12516 全部交付并通过全量门禁验收：沙箱信任边界、可恢复队列/SSE、配置质量一致性、真实原子导出、审阅草稿恢复与可访问性）；Linux 容器交付暂缓（12505 保持 blocked）。
- Phase 10 macOS Native Workbench UI 已完成。
- Phase 8 macOS Batch Task Center 已完成。
