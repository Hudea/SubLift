# SubLift 进度导航

## 当前状态

- **最后更新：** 2026-08-03
- **当前 Phase：** Phase 7 Harness Migration 已完成。
- **进度真源：** `phases.json → detail_file`；本文件仅作会话导航。
- **下一优先：** 等用户明确下一项产品范围后，在 `phases.json` 登记新的 canonical Feature；不重新启用历史 `feat-*` 作为当前任务。

## 进行中

- 无

## 近期完成

- [x] 07001：新 Harness 协议、Phase 索引与项目契约已接入。
- [x] legacy Phase 1–6 与 canonical Phase 7 的边界已冻结，历史小数 Phase 路径已规范化。
- [x] 会话 L0 与完整标准产品验证门已分离，旧调用入口保留兼容转发。
- [x] benchmark 仓库根识别已支持 `phases.json` 优先与 `feature-list.json` fallback。

## 阻塞项 / 风险

- Phase 2 的独立 `.app` 分发与公证按 ADR-0009 保持后置。
- Phase 3 的独立性能 baseline 任务保持 blocked，需新的多源数据与用户优先级决定。
- feat-06910–06912（bundle、签名/公证、最终 artifact Python-free 门）按 ADR-0030 整体后置。

## 近期决策

- ADR-0032：采用新 Harness，以兼容层保留历史追踪与完整产品门。
- ADR-0031：Benchmark 代码进入正式包，configs/datasets/baselines/debug 分层。
- ADR-0030：Phase 6.9 止于开发架构收口，产品分发整体后置。

> 完成证据见 `docs/phases/phase7.json`；历史功能证据仍在各 Phase detail 文件中。
