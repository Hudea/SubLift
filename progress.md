# SubLift 进度导航

## 当前状态

- **最后更新：** 2026-09-04
- **当前 Phase：** [Phase 14](docs/phases/phase14.json) — `ready-for-merge`（D01–D05 全部完成；合入 `main` 并复验前不置 `done`）
- **当前 Deliverable：** 无（等待合入）
- **已冻结里程碑：** [Phase 13](docs/phases/phase13.json) 已在 `main` 完成验收并置 `done`。
- **进度真源：** [`phases.json`](phases.json) → `detail_file`；本文件只提供当前工作与阻塞导航。

## 当前收口顺序

- Phase 14 把 Web / Native Server 以容器镜像交付到可信局域网服务器（[ADR-0040](docs/DECISIONS.md)）。
- 产品验证使用 `./scripts/verify-product.sh`（不依赖 Docker）；可选离线工具使用 `./scripts/verify-offline.sh`；
  容器验收使用独立的 `./scripts/verify-container.sh`（Python-free，需要 Docker daemon）。

## 阻塞项

- 无执行阻塞。合入 `main` 并在 `main` 上完成必要验收后，Phase 14 才置 `done`。
- 12505 — `blocked`：Phase 12 的历史记录，容器交付意图已由 Phase 14 承接，不再作为执行入口。

## 当前风险

- 局域网自托管改变了 Web 入口的信任边界：非 loopback 暴露必须同时具备媒体根、工作区锁定与
  可选 token。当前 Web UI 不会发送 Bearer，开启 `SUBLIFT_ACCESS_TOKEN` 会使浏览器工作台 401。

## 近期决策

- ADR-0040：容器化自托管限于可信局域网，宿主卷为媒体入口，非 loopback 受最小访问控制约束
  （2026-09-03）。GPU/CUDA、公网 HTTPS、账号与 CI/CD 均非本 Phase 目标。

## 最近收口

- Phase 14 D05 完成：README / `.env.example` / ARCHITECTURE 7.2 / REQUIREMENTS 与进度导航
  与真实边界对齐；容器门保持独立入口。
- Phase 14 D04 完成：`./scripts/verify-container.sh` 在本机 Docker 29.1.3 上 14/14 通过（paddle.available、/media Paddle 提取、golden SRT 精确比对、沙箱 400、工作区锁定 403）。
- Phase 14 D03 完成：服务端非 loopback 访问控制、docker-compose.yml 与 Web 锁定感知落地。
- Phase 14 D02 完成：本机 `docker build -t sublift:local` 成功（748MB，non-root `sublift`，ORT 1.18.1 与 PP-OCRv6 按 manifest 校验装入）。
- Phase 14 D01 完成：ADR-0040 冻结，Phase 14 登记并通过 Draft 2020-12 校验。
- Phase 13 已合入 `main`，通过 `./scripts/verify-product.sh` 并在 `main` 上冻结为 `done`。
