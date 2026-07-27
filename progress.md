# 会话进度日志

## 当前状态

- **最后更新：** 2026-07-27（6.0 审查整改：init.sh / int32 / parity 门）
- **当前 Phase：** phase6-native-cpp-core
- **子阶段 6.0：** **done**（feat-06001 ~ feat-06005；审查 P0/P1 已修）
- **下一刀：** Phase **6.1** pure pipeline（`feat-06101` signature）
- **分支：** `refactor/cpp`
- **说明：** 产品默认路径仍为 Python；C++ 地基与冻结 Oracle 就绪。

## 进行中

- 无

## 近期完成（最近 5 个）

- [x] **6.0 审查整改**：init.sh bash3.2 空数组；ImageBuffer/models int64；require_i32 范围；parse object 守卫；init 接入 dump_config --check；Config==；nlohmann 挪到 test_support（ctest 29/29）
- [x] **feat-06004** Parity harness + 冻结 Config oracle
- [x] **feat-06003** ImageBuffer / 强类型 box / 完整 Config
- [x] **feat-06002** C++ CMake multi-target + Catch2
- [x] **feat-06005 / 06001** 设计与文档

## 阻塞项 / 风险

- [ ] **merged residual / #15 砰**
- [ ] **ground truth 素材有限**
- [ ] **Paddle cutover**：6.6 后 paddle 仍 Python worker
- [ ] **C++ parity**：须复现历史行为怪癖（色域等）

## 近期决策

- **ADR-0021 P1 契约** + **ADR-0020 Phase 6**
- **6.0 完成定义**：地基就绪即可进 6.1，不切换产品路径

> 完整决策记录见 `docs/DECISIONS.md`
