# SubLift 进度导航

> 本文件只做导航，不沉积 evidence 或实施过程；细节在 Phase detail、`docs/HURDLES.md` 与
> `benchmark/baselines/`。进度真源：[`phases.json`](phases.json) → `detail_file`。

## 当前状态

- **最后更新：** 2026-10-22
- **当前 Phase：** [Phase 20](docs/phases/phase20.json) `ready-for-merge`（Paddle OCR
  算子优化，D01–D04 done；分支 `opt/paddle-operators`，待合入 main 并复验后置 done）；
  [Phase 18](docs/phases/phase18.json) `done`；[Phase 14](docs/phases/phase14.json)
  `blocked`（外部 Docker/容器运行时缺失，D01–D05 已 done，`verify-container` 无法执行）；
  [Phase 15](docs/phases/phase15.json) `blocked`（GPU 外部条件）
- **已登记待启动：** [Phase 19](docs/phases/phase19.json) `not-started`（Phase 17 拆出的
  Session/Layout/工具链收口）；[Phase 17](docs/phases/phase17.json) 已以 D01–D02 `done`
- **当前 Deliverable：** 无 in-progress；Phase 20 D01–D04 全部 `done`，待合并验收
- **最近收口：** [Phase 16](docs/phases/phase16.json) 以负向结论 `done`；
  [Phase 12](docs/phases/phase12.json) `done`（12505 为历史 blocked）；
  [Phase 15](docs/phases/phase15.json) 保持 `blocked`（GPU 外部条件）
- **验证入口：** `./scripts/verify-harness.sh`（真源/链接）· `./scripts/verify-product.sh`
  （产品门，Python-free）· `./scripts/verify-container.sh`（容器，独立）·
  `./scripts/verify-offline.sh`（可选 Python）

## 下一步

1. **Phase 20 收口**：分支 `opt/paddle-operators` 上 D01–D04 已完成、等价性与产品门
   证据就绪；合入 `main` 并在 `main` 上复验后置 `done`（用户批准前不推送）。
2. **Phase 14 复验（外部）**：需要 Docker/podman/colima 等容器运行时；本机均不可用，
   故 Phase 14 置 `blocked`，具备环境后运行 `verify-container` 复验即可。
3. **Phase 19（next）**：Web 队列持久化、ApplicationSession、前端分层、根目录收口。
4. **Phase 15（外部）**：GPU 真机条件，保持 `blocked`。

## 阻塞

- **Phase 15 D02–D04**：需目标显卡/驱动、原生 ORT GPU 包与 CUDA/cuDNN/基础镜像冻结，以及
  真实 NVIDIA GPU + Docker 环境；本机两者皆无。
- **Phase 14**：未在 `main` 上完成必要复验前不置 `done`；本机无 Docker 客户端/daemon，
  容器门无法执行，需要具备 Docker 的环境。

## 风险 / 已知残余

- **Web 批量队列恢复**：服务端 job 状态与 SSE cursor 已持久化，但 Web 批量队列的成员/顺序未
  跨刷新持久化；刷新后队列丢失。归入维护 Phase。
- **Web + token**：开启 `SUBLIFT_ACCESS_TOKEN` 时浏览器工作台不会发送 Bearer，打开即 401；
  容器自托管不要给 Web 开 token，或先补同源鉴权。
- **Paddle 性能天花板**：ORT 推理约占 core wall 75%（`det_infer` 约 65%）；Phase 20 已把
  C++ 算子侧相对成本压缩约 40%，实际端到端收益随字幕密度约 0.6–1.5%；进一步提速需处理
  det 调用次数/pipeline 侧算子或 GPU，均已超出本 Phase。
- **未登记分支**：`feat/llm-subtitle-correction`（独立 worktree）自带 Phase 9 字幕校正，
  未进入 `phases.json` 真源，待决定合并/归档。

## 最近收口

- **Phase 20（2026-10-22）**：Paddle OCR 算子优化 D01–D04 done；RapidOCR→C++ 审计、
  算子级归因，det 前处理 -40%、DB 后处理 -24%、CTC 解码 -94%；全片 SRT sha256 与 736 回滚
  hash 不变，CTest 281/281；分支 `opt/paddle-operators` 待合入 main 复验。
- **收尾（2026-10-22）**：清理本地实验 worktree（`.worktrees/det-geometry`、
  `/tmp/sublift-baseline`），保留质量门需要的 `/tmp/sublift-python-oracle`；测试视频
  `zoo_480p/zoo_720p` 已复制回 `debug/det-policy/`；Phase 14 因无容器运行时置 `blocked`。
- **Phase 18 D04（2026-10-22）**：产品默认切到 0.6MP（无 flag，无需 env/重建）；默认 1080p
  输出 `f77296e3…`（49.50s），`--det-pixel-budget 0` 回滚 `56b1b08a…`（138.20s，2.79×）；
  Phase 18 以 D01–D04 `done` 收口。
- **Phase 18 D02–D04（2026-10-22）**：历史 Oracle 重放、质量门 v2 重冻、720p/band 覆盖；
  0.6MP 默认切换（默认 `f77296e3…`，`--det-pixel-budget 0` 回滚 `56b1b08a…`），全门 PASS。
- **Phase 18 D01 / det 几何实验（2026-10-22）**：ADR-0042 获批；135.35s→45.00s（3.03×）；
  0.6MP 配置面落地，默认 736 hash 保持。
- **更早（Phase 16/17/12/14）**：见对应 Phase detail；Phase 16 负向结论 `done`、Phase 17
  合同对齐 `done`、Phase 12 `done`（12505 历史 blocked）、Phase 14 待 `main` 复验。
