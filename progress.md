# 会话进度日志

## 当前状态

- **最后更新：** 2026-07-30
- **当前 Phase：** Phase 6.9 Native 产品架构整理
- **分支 / worktree：** `refactor/native-product-architecture` @ `/Volumes/lab/pp/SubLift_CPP`
- **基线：** 6.8 已合入 `main`（`99e361e`）；Paddle available → C++ stable
- **计划：** `docs/cpp/phase6.9-implementation-plan.md`（feat-06901–06913）
- **目标态：** `docs/cpp/phase6.9-native-product-architecture.md`
- **下一实现：** `feat-06913`（Phase 6 终态清理与 Fallback 退役准备）

## 进行中

- 无

## 近期完成

- [x] **feat-06912**：Python-free Standalone Native 运行验证
- [x] **feat-06911**：Swift App Shell + Native Driver
- [x] **feat-06910**：macOS Product Bundle 布局
- [x] **feat-06909**：Native CLI 产品 parity
- [x] **feat-06908**：ResourceLocator 真实资源探查与 Probe=Construct 同一化

## 阻塞项 / 风险

- 正式 `.app` 签名/公证依赖证书（feat-06911 可能 blocked，不阻塞 06910 布局完成）。
- Fallback 退役（06913）须满足观察期，不可提前删除 Python 回滚。
- Release 全量 CTest 仍有既有 Vision synthetic 环境用例失败（与 6.9 结构工作无关）。

## 近期决策

- **6.9 执行顺序**：先 CMake target 边界 → 机械目录 → ModelBundle → 打包 → Python-free → fallback 退役
- **CLI 必须走 Worker**；不新增 in-process OCR 产品路径
- **ADR-0029**（仍有效）：Paddle 默认 C++ stable；Python 回滚保留至 06913

> 完成证据只写 `docs/phases/phase6.json`；长期决策见 `docs/DECISIONS.md`。
