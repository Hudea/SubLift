# 会话进度日志

## 当前状态

- **最后更新：** 2026-07-30
- **当前 Phase：** Phase 6.9 + feat-06914 Benchmark v2 重构完成
- **分支 / worktree：** `main` @ `/Volumes/lab/pp/SubLift`
- **下一优先：** 审阅本次改动；正式多源复测后再决定是否调整默认 fps

## 进行中

- 无

## 近期完成

- [x] Benchmark 可执行代码迁入 `src/sublift/benchmark/`，资产与本机产物分层
- [x] `sublift-benchmark` 统一 run/matrix/score/show/overhead/compare-roi
- [x] 通用 `--set/--vary` 与 `pipeline.*` 嵌套参数矩阵、聚合报告落地
- [x] 8 SRT + 2 log 迁入 `debug/benchmark/imports/` 并完成统一口径分析
- [x] ResourceLocator 基线修复，标准 `./init.sh` 10/10 通过

## 阻塞项 / 风险

- feat-06910–06912（bundle、签名/公证、最终 artifact Python-free 门）按
  ADR-0030 整体后置；不阻塞开发架构合入。
- 本轮 5/8/12fps 结论只有 Zootopia 单一片源，日志也是单次 wall；不能直接晋升
  产品默认，须补多源 warmup=1 + measured=3 与 RSS/CPU。
- 两份源文件名写作 10fps、日志与时间步长实际为 12fps；已在分析中按 12fps 标注，
  原始导入文件保留不改名。

## 近期决策

- ADR-0031：Benchmark 代码进入正式包，configs/datasets/baselines/debug 分层。
- Pipeline 调参统一使用 `pipeline.*` dotted path，不再增加一次性 scan 脚本。
- 现阶段保留默认 fps；Vision 8fps、Paddle 12fps 只作为下一轮复测候选。

> 验证证据见 `docs/phases/phase6.json`。
