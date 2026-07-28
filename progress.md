# 会话进度日志

## 当前状态

- **最后更新：** 2026-07-28（Phase 6.5 C++ Worker 全部完成）
- **当前 Phase：** phase6-native-cpp-core
- **子阶段 6.0–6.5：** **done**
- **子阶段 6.6：** **not-started**（6.6+ Cutover / 去 Python 产品 runtime）
- **下一刀：** Phase 6.6 交付准备
- **分支：** `refactor/cpp`
- **说明：** 产品默认路径仍为 Python。6.5 交付可 opt-in 的 C++ worker；默认 cutover 归 6.6。paddle 不进 C++。

## 进行中

- [x] Phase 6.5 完美交付！

## 近期完成（最近 5 个）

- [x] **feat-06505** Vision 装配 + IPC 夹具 / worker CLI (EngineFactory vision + test_cpp_worker.py + dump_ipc_session.py)
- [x] **feat-06504** Cancel / 断连 / frame mode 最小集 (BridgeHandler base64/CoreGraphics decode + frame mode + safe_push_cb)
- [x] **feat-06503** 连接循环 + path mode Mock (EngineFactory + BridgeHandler + WorkerConnection + main CLI)
- [x] **feat-06502** 协议类型 + capability 握手 (12类IPC DTO + json parse/serialize + build_bye)
- [x] **feat-06501** UDS 分帧编解码 (>I 4B BE + 64MB + EINTR retry + sublift_ipc)

## 阻塞项 / 风险

- [ ] **merged residual / #15 砰**
- [ ] **ground truth 素材有限**
- [ ] **Paddle cutover**：6.6 后 paddle 仍 Python worker
- [ ] **6.5 Worker：** 写 socket 串行化；frame mode 可 defer 但不得静默当 path；勿误切 GUI 默认

## 近期决策

- **6.5 五刀：** framing → protocol/capability → path+mock → cancel/frame → vision+夹具+CLI
- **6.5 默认仍 Python**；C++ worker 双轨 opt-in；capability 诚实无 paddle
- **6.4 review：** ARC 必开；跟踪以 phase6.json evidence 为准

> 完整决策记录见 `docs/DECISIONS.md`
