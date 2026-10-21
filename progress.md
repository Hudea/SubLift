# SubLift 进度导航

> 本文件只做导航，不沉积 evidence 或实施过程；细节在 Phase detail、`docs/HURDLES.md` 与
> `benchmark/baselines/`。进度真源：[`phases.json`](phases.json) → `detail_file`。

## 当前状态

- **最后更新：** 2026-10-21
- **当前 Phase：** [Phase 17](docs/phases/phase17.json) `in-progress`；
  [Phase 14](docs/phases/phase14.json) 保持 `ready-for-merge`（detail 已在 `main`，
  D01–D05 全 done；待 `main` 复验后置 `done`）
- **已登记待启动：** [Phase 18](docs/phases/phase18.json) `not-started`（det 输入几何
  契约与默认切换；D01 先批准 ADR-0042，契约通过前默认仍为 736）
- **当前 Deliverable：** Phase 17 / D02「跨端语义对齐」
  （in-progress；SRT 与输出重名已对齐，剩余 confidence/ROI/状态映射）
- **最近收口：** [Phase 16](docs/phases/phase16.json) 以负向结论 `done`；
  [Phase 12](docs/phases/phase12.json) `done`（12505 为历史 blocked）；
  [Phase 15](docs/phases/phase15.json) 保持 `blocked`（GPU 外部条件）
- **验证入口：** `./scripts/verify-harness.sh`（真源/链接）· `./scripts/verify-product.sh`
  （产品门，Python-free）· `./scripts/verify-container.sh`（容器，独立）·
  `./scripts/verify-offline.sh`（可选 Python）

## 下一步

1. **D02 收尾**：SRT 与输出重名已对齐；下一步处理 confidence 默认值、ROI 转换与
   Job/Task 状态映射，仍以 contracts fixtures + 三端测试为准。
2. **D03 启动**：Web 批量队列版本化持久化/恢复；优先复用 Swift BatchQueueState 语义，
   再决定 localStorage 还是服务端 queue endpoint。
3. **Phase 14 收口**：在具备 Docker 的环境于 `main` 上运行 `verify-product` 与
   `verify-container`；通过后置 `done`。本机当前没有 Docker（`docker: command not found`），
   不能伪造复验。
4. **Phase 18 / D01**：批准 det 几何契约 ADR（ADR-0042 草案），冻结策略与质量门阈值；
   实验证据见 `docs/design/det-pixel-budget-delivery.md`，默认保持 736。

## 阻塞

- **Phase 15 D02–D04**：需目标显卡/驱动、原生 ORT GPU 包与 CUDA/cuDNN/基础镜像冻结，以及
  真实 NVIDIA GPU + Docker 环境；本机两者皆无。
- **Phase 14**：未在 `main` 上完成必要复验前不置 `done`；本机无 Docker 客户端/daemon，
  容器门无法执行，需要具备 Docker 的环境。

## 风险 / 已知残余

- **Web 批量队列恢复**：服务端 job 状态与 SSE cursor 已持久化，但 Web 批量队列的成员/顺序未
  跨刷新持久化；刷新后队列丢失。归入维护 Phase。
- **跨端默认值与状态**：SRT 与输出重名已对齐；confidence 默认值（Swift 0.5 vs 合同 0.0）、
  批量 ROI、Job/Task 状态映射仍未统一。归入 D02。
- **Web + token**：开启 `SUBLIFT_ACCESS_TOKEN` 时浏览器工作台不会发送 Bearer，打开即 401；
  容器自托管不要给 Web 开 token，或先补同源鉴权。
- **工作树**：`exp/det-pixel-budget` 的 det 几何重构已提交（09a9137）；主工作树干净。
  Phase 18 的契约与默认切换尚未开始，不要在该分支直接改默认值。

## 最近收口

- **det 几何实验（2026-10-21）**：`exp/det-pixel-budget` 提交 09a9137；固定负载
  136.35s → 45.00s（3.03×），默认仍为 736；质量与 parity 风险见交付报告，Phase 18 承接决策。
- **Phase 17（2026-10-20）**：D01 合同/parity 收口；D02 已对齐 SRT 与输出重名，
  三端共享 fixtures 测试通过。下一步 confidence/ROI/状态映射。
- **Phase 16（2026-10-20）**：P0 的 S0 基线与 O1–O8 全部落地；端到端收益在噪声内，det_infer
  占 84.35%，以负向结论收口。P1/P2 未启动，后续另立 Phase。
- **Phase 12（2026-10-20）**：12509–12516 全部交付；12505 历史 blocked 由 Phase 14 承接。
  复核发现 Web 批量队列持久化残余，记入上方风险。
- **Phase 14（2026-09-03/04）**：D01–D05 完成并合入 `main`；等待 `main` 复验。
