# SubLift 进度导航

> 本文件只做导航，不沉积 evidence 或实施过程；细节在 Phase detail、`docs/HURDLES.md` 与
> `benchmark/baselines/`。进度真源：[`phases.json`](phases.json) → `detail_file`。

## 当前状态

- **最后更新：** 2026-10-22
- **当前 Phase：** [Phase 18](docs/phases/phase18.json) `in-progress`；
  [Phase 14](docs/phases/phase14.json) 保持 `ready-for-merge`（detail 已在 `main`，
  D01–D05 全 done；待 `main` 复验后置 `done`）
- **已登记待启动：** [Phase 19](docs/phases/phase19.json) `not-started`（Phase 17 拆出的
  Session/Layout/工具链收口）；[Phase 17](docs/phases/phase17.json) 已以 D01–D02 `done`
- **当前 Deliverable：** Phase 18 / D03「覆盖矩阵、碎片合并与全门质量验证」
  （in-progress；D02 基线重冻完成，0.6MP 全门 PASS）
- **最近收口：** [Phase 16](docs/phases/phase16.json) 以负向结论 `done`；
  [Phase 12](docs/phases/phase12.json) `done`（12505 为历史 blocked）；
  [Phase 15](docs/phases/phase15.json) 保持 `blocked`（GPU 外部条件）
- **验证入口：** `./scripts/verify-harness.sh`（真源/链接）· `./scripts/verify-product.sh`
  （产品门，Python-free）· `./scripts/verify-container.sh`（容器，独立）·
  `./scripts/verify-offline.sh`（可选 Python）

## 下一步

1. **Phase 18 D03**：720p 从 1080p 下采样构造（标注非独立源）、左边界 guard、覆盖矩阵与全门质量。
2. **Phase 18 D04**：默认切换、回滚演练与收口。
3. **Phase 14 收口**：在具备 Docker 的环境于 `main` 上运行 `verify-product` 与
   `verify-container`；通过后置 `done`。本机当前没有 Docker（`docker: command not found`）。
4. **Phase 19（backlog）**：Web 队列持久化、ApplicationSession、前端分层、根目录收口。

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
- **工作树**：det 几何实验与去重修复已合入 `main`（4fe4fa5）；工作树干净。
  产品默认仍为 736；Phase 18 的契约与默认切换尚未开始。

## 最近收口

- **Phase 18 D02（2026-10-22）**：历史 Oracle 重放精确复现 v1 指标；gate 改为
  `--oracle-worktree` + 质量门；v2 基线重冻；0.6MP 候选全门 PASS，timing F1/precision、CER、usable 均优于
  Oracle，三来源 wall 约 3.0–3.2×。
- **Phase 18 D01（2026-10-22）**：ADR-0042 获批；0.6MP 配置面落地（CLI `--det-pixel-budget`、
  Server `det_pixel_budget`）；默认 736 hash `56b1b08a…` 不变，env 降级为 adapter 诊断。
- **det 几何实验（2026-10-22）**：实验、去重修复与 Phase 18 计划已合入 `main`（4fe4fa5）；
  固定负载 136.35s → 45.00s（3.03×），默认仍为 736；720p 0.6 经去重后 precision
  89.4% → 95.1%、F1 88.4% → 92.3%，736 hash 保持 `56b1b08a…`。
- **Phase 17 D02（2026-10-22）**：SRT、输出重名、confidence、ROI、Job/Task 状态与
  engine/script 默认值全部对齐 contracts；C++ ctest 273/273、Web 159 tests、Swift 143 tests。
- **Phase 17（2026-10-22）**：D02 收口，Phase 17 以 D01–D02 `done`；Session/Layout/工具链
  拆出为 [Phase 19](docs/phases/phase19.json) `not-started`；Phase 18 置 `in-progress`。
- **Phase 16（2026-10-20）**：P0 的 S0 基线与 O1–O8 全部落地；端到端收益在噪声内，det_infer
  占 84.35%，以负向结论收口。P1/P2 未启动，后续另立 Phase。
- **Phase 12（2026-10-20）**：12509–12516 全部交付；12505 历史 blocked 由 Phase 14 承接。
  复核发现 Web 批量队列持久化残余，记入上方风险。
- **Phase 14（2026-09-03/04）**：D01–D05 完成并合入 `main`；等待 `main` 复验。
