# 会话进度日志

## 当前状态

- **最后更新：** 2026-07-27（6.1 独立审查加固）
- **当前 Phase：** phase6-native-cpp-core
- **子阶段 6.0：** **done**
- **子阶段 6.1：** **done**（feat-06101–06105 + 审查加固）
- **下一刀：** Phase **6.2** Pipeline 流式 API（`feat-062xx`，开干前拆任务）
- **分支：** `refactor/cpp`
- **说明：** 产品默认路径仍为 Python。

## 进行中

- 无

## 近期完成（最近 5 个）

- [x] **6.1 审查加固**：cleanup/SSIM veto/dedupe multipass/Unicode ws/NFC compare/CHANGE fail-fast；ctest 71/71
- [x] **feat-06105** Line select pure API parity
- [x] **feat-06104** Dedupe merge_entries parity
- [x] **feat-06103** TimelineBuilder parity
- [x] **feat-06102** Changepoint detector parity

## 阻塞项 / 风险

- [ ] **merged residual / #15 砰**
- [ ] **ground truth 素材有限**
- [ ] **Paddle cutover**：6.6 后 paddle 仍 Python worker
- [ ] **C++ parity**：色域/timestamp falsy-0 等怪癖须继续复现

## 近期决策

- **6.1 完成**：pure pipeline 五模块 C++ + golden；产品路径未切换
- **timestamp_ms=0 falsy**：复现 Python `or` 语义（06102）
- **edit_distance UTF-8 codepoint**：对齐 Python 字符串语义（06105）

> 完整决策记录见 `docs/DECISIONS.md`
